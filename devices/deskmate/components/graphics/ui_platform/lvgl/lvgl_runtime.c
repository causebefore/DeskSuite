/*
 * 文件职责：封装 LVGL tick、display flush 和运行时锁。
 */
#include "ui_platform_lvgl.h"

#include <stdint.h>
#include <string.h>

#include "device_display.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define LVGL_PERF_LOG_INTERVAL_US (5 * 1000 * 1000)
#define LVGL_PORT_STOP_TIMEOUT_MS 1000U
/* 略大于 MIP 面板约 40ms 的 TE 周期，避免动画帧挤压 BSP 待发送批次。 */
#define LVGL_RENDER_PERIOD_MS     45U
/* 当前托管 esp_lvgl_port 以固定名称创建内部 Task；升级组件时必须复审该退出屏障。 */
#define LVGL_PORT_TASK_NAME       "taskLVGL"

static const char *TAG = "ui_platform";

typedef struct
{
    uint32_t flush_count;
    uint32_t frame_count;
    uint32_t interval_min_us;
    uint32_t interval_max_us;
    uint64_t interval_us;
    uint64_t flush_cb_us;
} lvgl_perf_stats_t;

static lv_display_t     *s_display;
static lv_timer_t       *s_render_timer;
static void             *s_draw_buffer_1;
static void             *s_draw_buffer_2;
static bool              s_initialized;
static bool              s_port_started;
static bool              s_port_stop_requested;
static bool              s_suspended;
static int64_t           s_last_flush_enter_us;
static int64_t           s_perf_window_start_us;
static lvgl_perf_stats_t s_perf_stats;
static uint32_t          s_total_frames;

/**
 * @brief 把绝对微秒期限换算为至少一个 FreeRTOS Tick
 */
static TickType_t ticks_until(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0)
    {
        return 0;
    }
    TickType_t ticks = pdMS_TO_TICKS((remaining_us + 999LL) / 1000LL);
    return ticks == 0 ? 1 : ticks;
}

/**
 * @brief 首次停止时在同步对象仍存活期间唤醒 Task，并只提交一次异步停止请求
 *
 * @return ESP_OK 已提交或此前已经提交；其他值表示首次停止请求失败
 */
static esp_err_t request_port_stop(void)
{
    if (!s_port_started)
    {
        s_port_stop_requested = false;
        return ESP_OK;
    }
    if (s_port_stop_requested)
    {
        return ESP_OK;
    }

    /*
     * 必须在 port 仍为运行态时唤醒：此时 taskLVGL 不会进入资源清理，
     * lvgl_events 的生命周期有保证。停止请求发出后不得再访问 port 的同步对象。
     */
    const esp_err_t wake_err = lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
    if (wake_err != ESP_OK && wake_err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "停止前唤醒 LVGL port Task 失败: %s", esp_err_to_name(wake_err));
    }
    const esp_err_t deinit_err = lvgl_port_deinit();
    if (deinit_err != ESP_OK)
    {
        return deinit_err;
    }
    s_port_stop_requested = true;
    return ESP_OK;
}

/** @brief 把绝对期限转换为向上取整的剩余毫秒数 */
static uint32_t remaining_timeout_ms(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0)
    {
        return 0;
    }
    const uint64_t remaining_ms = ((uint64_t) remaining_us + 999ULL) / 1000ULL;
    return remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t) remaining_ms;
}

/**
 * @brief 请求 LVGL port Task 停止并等待第三方异步清理真正完成
 */
static esp_err_t stop_port_and_wait(uint32_t timeout_ms)
{
    if (!s_port_started)
    {
        s_port_stop_requested = false;
        return ESP_OK;
    }
    if (timeout_ms == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t   deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    const esp_err_t stop_err    = request_port_stop();
    if (stop_err != ESP_OK)
    {
        return stop_err;
    }
    while (xTaskGetHandle(LVGL_PORT_TASK_NAME) != NULL)
    {
        /*
         * 当前托管 port 在启动通知后才写 running=true；初始化回滚可能先提交停止，
         * 随后又被该迟到写入覆盖。deinit 只更新静态 running 标志，可在等待期间
         * 幂等重申，不访问已经可能释放的 EventGroup 或互斥量。
         */
        (void) lvgl_port_deinit();
        const TickType_t wait_ticks = ticks_until(deadline_us);
        if (wait_ticks == 0)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(wait_ticks > 1 ? 1 : wait_ticks);
    }
    s_port_started        = false;
    s_port_stop_requested = false;
    return ESP_OK;
}

/**
 * @brief 在 LVGL port 已启动但平台初始化未完成时停止后台任务
 */
static esp_err_t rollback_port_init(esp_err_t original_error)
{
    const esp_err_t rollback_err = stop_port_and_wait(LVGL_PORT_STOP_TIMEOUT_MS);
    if (rollback_err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "回滚 esp_lvgl_port 初始化失败: original=%s rollback=%s",
                 esp_err_to_name(original_error),
                 esp_err_to_name(rollback_err));
    }
    const esp_err_t display_error = device_display_deinit();
    if (display_error != ESP_OK && display_error != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "回滚显示设备初始化失败: %s", esp_err_to_name(display_error));
    }
    return original_error;
}

static void perf_record_enter(int64_t now_us)
{
    if (s_last_flush_enter_us > 0)
    {
        const uint32_t interval_us = (uint32_t) (now_us - s_last_flush_enter_us);
        s_perf_stats.interval_us += interval_us;
        if (s_perf_stats.interval_min_us == 0 || interval_us < s_perf_stats.interval_min_us)
        {
            s_perf_stats.interval_min_us = interval_us;
        }
        if (interval_us > s_perf_stats.interval_max_us)
        {
            s_perf_stats.interval_max_us = interval_us;
        }
    }
    s_last_flush_enter_us = now_us;
}

static void perf_log_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_perf_window_start_us == 0)
    {
        s_perf_window_start_us = now_us;
        return;
    }
    const int64_t elapsed_us = now_us - s_perf_window_start_us;
    if (elapsed_us < LVGL_PERF_LOG_INTERVAL_US || s_perf_stats.flush_count == 0)
    {
        return;
    }

    const uint32_t flushes = s_perf_stats.flush_count;
    ESP_LOGI(TAG,
             "LVGL 性能: fps=%lu frames=%lu flushes=%lu flush_cb_avg_us=%llu",
             (unsigned long) ((uint64_t) s_perf_stats.frame_count * 1000000ULL / (uint64_t) elapsed_us),
             (unsigned long) s_perf_stats.frame_count,
             (unsigned long) flushes,
             (unsigned long long) (s_perf_stats.flush_cb_us / flushes));
    memset(&s_perf_stats, 0, sizeof(s_perf_stats));
    s_perf_window_start_us = now_us;
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    const int64_t start_us = esp_timer_get_time();
    perf_record_enter(start_us);

    lv_draw_buf_t *draw_buf     = lv_display_get_buf_active(display);
    const uint32_t stride       = draw_buf->header.stride;
    const size_t   palette_size = LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * sizeof(lv_color32_t);

    esp_err_t err               = device_display_write_i1_area((int) area->x1,
                                                               (int) area->y1,
                                                               (int) area->x2,
                                                               (int) area->y2,
                                                               px_map + palette_size,
                                                               stride);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "写入 RLCD framebuffer 失败: %s", esp_err_to_name(err));
    }

    if (lv_display_flush_is_last(display))
    {
        err = device_display_request_flush();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "刷新 RLCD 失败: %s", esp_err_to_name(err));
        }
        s_perf_stats.frame_count++;
    }
    s_total_frames++;
    lv_display_flush_ready(display);
    s_perf_stats.flush_count++;
    s_perf_stats.flush_cb_us += (uint64_t) (esp_timer_get_time() - start_us);
    perf_log_if_due();
}

/**
 * @brief 在 taskLVGL 上提交当前脏区，并按动画数量决定是否继续周期渲染
 */
static void render_timer_cb(lv_timer_t *timer)
{
    if (s_display == NULL || s_suspended)
    {
        lv_timer_pause(timer);
        return;
    }

    lv_refr_now(s_display);
    if (lv_anim_count_running() == 0)
    {
        lv_timer_pause(timer);
    }
}

bool ui_platform_lvgl_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void ui_platform_lvgl_unlock(void)
{
    lvgl_port_unlock();
}

esp_err_t ui_platform_lvgl_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    if (s_port_started)
    {
        ESP_RETURN_ON_ERROR(stop_port_and_wait(LVGL_PORT_STOP_TIMEOUT_MS), TAG, "等待上一次 LVGL port Task 退出失败");
    }

    ESP_RETURN_ON_ERROR(device_display_init(), TAG, "初始化显示设备失败");
    device_display_info_t display_info;
    const esp_err_t       display_info_error = device_display_get_info_copy(&display_info);
    if (display_info_error != ESP_OK)
    {
        (void) device_display_deinit();
        return display_info_error;
    }

    lv_init();
    lvgl_port_cfg_t port_cfg   = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority     = 4;
    port_cfg.task_affinity     = 0;
    /* 活跃动画由 LVGL 最近 timer deadline 唤醒；空闲时允许任务等待事件。 */
    port_cfg.task_max_sleep_ms = 30000;
    port_cfg.timer_period_ms   = 5;
    const esp_err_t port_err   = lvgl_port_init(&port_cfg);
    s_port_started             = xTaskGetHandle(LVGL_PORT_TASK_NAME) != NULL;
    /* lvgl_port_init() 失败返回前已自行提交 deinit，不得再次触碰其同步对象。 */
    s_port_stop_requested      = port_err != ESP_OK && s_port_started;
    if (port_err != ESP_OK)
    {
        return rollback_port_init(port_err);
    }
    s_port_started = true;

    if (!ui_platform_lvgl_lock(UINT32_MAX))
    {
        ESP_LOGE(TAG, "获取 LVGL 锁失败");
        return rollback_port_init(ESP_ERR_TIMEOUT);
    }
    s_display = lv_display_create(display_info.width_pixels, display_info.height_pixels);
    if (s_display == NULL)
    {
        ui_platform_lvgl_unlock();
        return rollback_port_init(ESP_ERR_NO_MEM);
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_I1);
    lv_display_set_flush_cb(s_display, flush_cb);

    const size_t buffer_size = (size_t) display_info.width_pixels * display_info.height_pixels / 8U + 1024U;
    s_draw_buffer_1          = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_draw_buffer_2          = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_draw_buffer_1 == NULL || s_draw_buffer_2 == NULL)
    {
        heap_caps_free(s_draw_buffer_1);
        heap_caps_free(s_draw_buffer_2);
        s_draw_buffer_1 = NULL;
        s_draw_buffer_2 = NULL;
        lv_display_delete(s_display);
        s_display = NULL;
        ui_platform_lvgl_unlock();
        return rollback_port_init(ESP_ERR_NO_MEM);
    }

    lv_display_set_buffers(s_display, s_draw_buffer_1, s_draw_buffer_2, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(s_display);
    /* 平台默认进入按需模式，避免 ui_main 建树前先周期性渲染空白屏。 */
    lv_timer_t *refresh_timer = lv_display_get_refr_timer(s_display);
    if (refresh_timer != NULL)
    {
        lv_timer_pause(refresh_timer);
    }
    s_render_timer = lv_timer_create(render_timer_cb, LVGL_RENDER_PERIOD_MS, NULL);
    if (s_render_timer == NULL)
    {
        heap_caps_free(s_draw_buffer_1);
        heap_caps_free(s_draw_buffer_2);
        s_draw_buffer_1 = NULL;
        s_draw_buffer_2 = NULL;
        lv_display_delete(s_display);
        s_display = NULL;
        ui_platform_lvgl_unlock();
        return rollback_port_init(ESP_ERR_NO_MEM);
    }
    lv_timer_pause(s_render_timer);
    ui_platform_lvgl_unlock();

    s_initialized = true;
    s_suspended   = false;
    ESP_LOGI(TAG, "LVGL 初始化完成: draw_buffer=%u bytes x2", (unsigned) buffer_size);
    return ESP_OK;
}

esp_err_t ui_platform_lvgl_stop(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0U, ESP_ERR_INVALID_ARG, TAG, "LVGL 停止超时无效");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "LVGL 平台尚未初始化");
    if (s_suspended)
    {
        return ESP_OK;
    }

    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    ESP_RETURN_ON_FALSE(ui_platform_lvgl_lock(timeout_ms), ESP_ERR_TIMEOUT, TAG, "停止前获取 LVGL 锁失败");

    if (s_render_timer != NULL)
    {
        lv_timer_pause(s_render_timer);
    }
    const esp_err_t port_error = lvgl_port_stop();
    if (port_error != ESP_OK)
    {
        if (s_render_timer != NULL)
        {
            lv_timer_resume(s_render_timer);
            lv_timer_ready(s_render_timer);
        }
        ui_platform_lvgl_unlock();
        return port_error;
    }

    const uint32_t  display_timeout_ms = remaining_timeout_ms(deadline_us);
    const esp_err_t display_error =
        display_timeout_ms == 0U ? ESP_ERR_TIMEOUT : device_display_stop(display_timeout_ms);
    if (display_error != ESP_OK)
    {
        const esp_err_t resume_error = lvgl_port_resume();
        if (s_render_timer != NULL)
        {
            lv_timer_resume(s_render_timer);
            lv_timer_ready(s_render_timer);
        }
        ui_platform_lvgl_unlock();
        if (resume_error == ESP_OK)
        {
            (void) lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
        }
        return resume_error == ESP_OK ? display_error : resume_error;
    }

    s_suspended = true;
    ui_platform_lvgl_unlock();
    ESP_LOGI(TAG, "LVGL 平台已停止，运行时资源保持");
    return ESP_OK;
}

esp_err_t ui_platform_lvgl_start(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0U, ESP_ERR_INVALID_ARG, TAG, "LVGL 恢复超时无效");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "LVGL 平台尚未初始化");
    if (!s_suspended)
    {
        return ESP_OK;
    }

    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    ESP_RETURN_ON_FALSE(ui_platform_lvgl_lock(timeout_ms), ESP_ERR_TIMEOUT, TAG, "恢复前获取 LVGL 锁失败");

    if (s_display == NULL)
    {
        ui_platform_lvgl_unlock();
        ESP_LOGE(TAG, "恢复时 LVGL 显示对象不存在");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = device_display_start();
    if (error != ESP_OK)
    {
        ui_platform_lvgl_unlock();
        return error;
    }

    const uint32_t flush_count_before = device_display_get_total_flush_count();
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(s_display);

    const uint32_t refresh_timeout_ms = remaining_timeout_ms(deadline_us);
    error = refresh_timeout_ms == 0U ? ESP_ERR_TIMEOUT : device_display_wait_flush_done(refresh_timeout_ms);
    if (error == ESP_OK && device_display_get_total_flush_count() == flush_count_before)
    {
        ESP_LOGE(TAG, "恢复时未完成任何显示刷新");
        error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error == ESP_OK)
    {
        error = lvgl_port_resume();
    }
    if (error != ESP_OK)
    {
        const uint32_t rollback_timeout_ms = remaining_timeout_ms(deadline_us);
        if (rollback_timeout_ms > 0U)
        {
            (void) device_display_stop(rollback_timeout_ms);
        }
        ui_platform_lvgl_unlock();
        return error;
    }

    if (s_render_timer != NULL)
    {
        if (lv_anim_count_running() > 0U)
        {
            lv_timer_resume(s_render_timer);
            lv_timer_ready(s_render_timer);
        }
        else
        {
            lv_timer_pause(s_render_timer);
        }
    }
    s_suspended = false;
    ui_platform_lvgl_unlock();

    const esp_err_t wake_error = lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
    if (wake_error != ESP_OK)
    {
        ESP_LOGE(TAG, "恢复后唤醒 LVGL port Task 失败: %s", esp_err_to_name(wake_error));
        return wake_error;
    }
    ESP_LOGI(TAG, "LVGL 平台已恢复并完成一次完整刷新");
    return ESP_OK;
}

esp_err_t ui_platform_lvgl_deinit(uint32_t timeout_ms)
{
    if (!s_initialized && !s_port_started)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "LVGL 反初始化超时无效");
    if (!s_initialized)
    {
        const esp_err_t port_error    = stop_port_and_wait(timeout_ms);
        const esp_err_t display_error = device_display_deinit();
        return port_error != ESP_OK ? port_error : display_error;
    }

    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    ESP_RETURN_ON_FALSE(ui_platform_lvgl_lock(timeout_ms), ESP_ERR_TIMEOUT, TAG, "反初始化前获取 LVGL 锁失败");
    if (s_render_timer != NULL)
    {
        lv_timer_delete(s_render_timer);
        s_render_timer = NULL;
    }
    lv_display_delete(s_display);
    s_display = NULL;
    heap_caps_free(s_draw_buffer_1);
    heap_caps_free(s_draw_buffer_2);
    s_draw_buffer_1 = NULL;
    s_draw_buffer_2 = NULL;
    ui_platform_lvgl_unlock();
    s_initialized                    = false;

    const TickType_t remaining_ticks = ticks_until(deadline_us);
    if (remaining_ticks == 0)
    {
        (void) request_port_stop();
        return ESP_ERR_TIMEOUT;
    }
    const uint64_t  remaining_ms = ((uint64_t) remaining_ticks * (uint64_t) portTICK_PERIOD_MS);
    const esp_err_t port_error   = stop_port_and_wait(remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t) remaining_ms);
    const esp_err_t display_error = device_display_deinit();
    if (port_error == ESP_OK && display_error == ESP_OK)
    {
        s_suspended = false;
    }
    return port_error != ESP_OK ? port_error : display_error;
}

esp_err_t ui_platform_lvgl_request_refresh(void)
{
    if (s_display == NULL || s_render_timer == NULL || s_suspended)
    {
        return ESP_ERR_INVALID_STATE;
    }
    lv_timer_resume(s_render_timer);
    lv_timer_ready(s_render_timer);
    return lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
}

uint32_t ui_platform_lvgl_get_refresh_period(void)
{
    return LVGL_RENDER_PERIOD_MS;
}

uint32_t ui_platform_lvgl_get_flush_fps(void)
{
    return device_display_get_flush_fps();
}

uint32_t ui_platform_lvgl_get_total_frames(void)
{
    return s_total_frames;
}

uint32_t ui_platform_lvgl_get_total_flush_count(void)
{
    return device_display_get_total_flush_count();
}
