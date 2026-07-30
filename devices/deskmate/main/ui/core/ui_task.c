/*
 * 文件职责：显式拥有产品 UI Task、业务/控制队列、状态机和固定控制回执。
 */
#include "ui_runtime.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "presentation_dispatch.h"
#include "sdkconfig.h"
#include "ui_main.h"
#include "ui_platform_font.h"
#include "ui_platform_lvgl.h"

#define UI_TASK_MESSAGE_QUEUE_LEN   16
#define UI_TASK_CONTROL_QUEUE_LEN   2
#define UI_TASK_STACK               CONFIG_DESKMATE_UI_TASK_STACK_SIZE
#define UI_TASK_PRIORITY            3
#define UI_TASK_ROLLBACK_TIMEOUT_MS 1000

typedef enum
{
    UI_TASK_CONTROL_START = 0,
    UI_TASK_CONTROL_STOP,
    UI_TASK_CONTROL_DEINIT,
} ui_task_control_type_t;

typedef struct
{
    ui_task_control_type_t type;
    uint32_t               request_id;
    int64_t                deadline_us;
} ui_task_control_t;

static const char *TAG = "ui_task";

static QueueHandle_t s_message_queue;
static QueueHandle_t s_control_queue;
static TaskHandle_t  s_task;
static bool          s_presentation_handler_registered;

static portMUX_TYPE       s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static ui_runtime_state_t s_state      = UI_RUNTIME_STATE_UNINIT;
static bool               s_business_open;
static uint32_t           s_active_posters;
static bool               s_resume_refresh_pending;
static bool               s_pending_page_switch_valid;
static ui_msg_t           s_pending_page_switch;
static bool               s_status_update_pending;

static StaticSemaphore_t s_control_mutex_storage;
static StaticSemaphore_t s_control_signal_storage;
static SemaphoreHandle_t s_control_mutex;
static SemaphoreHandle_t s_control_signal;
static portMUX_TYPE      s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t          s_next_request_id;
static uint32_t          s_completed_request_id;
static esp_err_t         s_completed_result;

static StaticSemaphore_t         s_start_signal_storage;
static SemaphoreHandle_t         s_start_signal;
static int64_t                   s_start_deadline_us;
static bool                      s_start_cancelled;
static bool                      s_start_completed;
static esp_err_t                 s_start_result;
static bool                      s_runtime_initialized;
static ui_user_intent_callback_t s_user_intent_callback;
static void                     *s_user_intent_context;

static esp_err_t          deinitialize_runtime(uint32_t timeout_ms);
static ui_runtime_state_t get_state(void);
static esp_err_t          post_message(const ui_msg_t *message, uint32_t timeout_ms);
static esp_err_t          request_control(ui_task_control_type_t type, uint32_t timeout_ms);
static esp_err_t          post_status_update_marker(void);
static void               end_business_post(void);

/**
 * @brief 在 UI 入口关闭期间合并恢复所需的最小呈现事实
 *
 * 普通状态事件只保留一枚刷新标记；页面切换额外保留最后一个目标。设置菜单动作属于瞬时
 * 物理输入，按键扫描停止后不应产生，因此不跨停止边界重放。
 */
static void merge_message_for_resume(const ui_msg_t *message)
{
    if (message == NULL || message->type == UI_MSG_SETTINGS_ACTION)
    {
        return;
    }
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != UI_RUNTIME_STATE_UNINIT && s_state != UI_RUNTIME_STATE_FAILED)
    {
        s_resume_refresh_pending = true;
        if (message->type == UI_MSG_SWITCH_PAGE)
        {
            s_pending_page_switch       = *message;
            s_pending_page_switch_valid = true;
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

typedef struct
{
    bool     refresh_pending;
    bool     page_switch_valid;
    ui_msg_t page_switch;
} ui_resume_snapshot_t;

/** @brief 原子取走当前恢复合并事实，后续到达的事件进入下一份合并状态 */
static ui_resume_snapshot_t take_resume_snapshot(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const ui_resume_snapshot_t snapshot = {
        .refresh_pending   = s_resume_refresh_pending,
        .page_switch_valid = s_pending_page_switch_valid,
        .page_switch       = s_pending_page_switch,
    };
    s_resume_refresh_pending    = false;
    s_pending_page_switch_valid = false;
    taskEXIT_CRITICAL(&s_state_lock);
    return snapshot;
}

/** @brief 重同步失败时把尚未应用的恢复事实与期间新到事件重新合并 */
static void restore_resume_snapshot(const ui_resume_snapshot_t *snapshot)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_resume_refresh_pending |= snapshot->refresh_pending;
    if (snapshot->page_switch_valid && !s_pending_page_switch_valid)
    {
        s_pending_page_switch       = snapshot->page_switch;
        s_pending_page_switch_valid = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

/**
 * @brief 把 UI 消息类型转换为稳定日志名称
 */
static const char *message_name(ui_msg_type_t type)
{
    switch (type)
    {
        case UI_MSG_NONE:
            return "NONE";
        case UI_MSG_SWITCH_PAGE:
            return "SWITCH_PAGE";
        case UI_MSG_STATUS_BAR_UPDATE:
            return "STATUS_BAR_UPDATE";
        case UI_MSG_STATUS_UPDATE:
            return "STATUS_UPDATE";
        case UI_MSG_POMODORO_UPDATE:
            return "POMODORO_UPDATE";
        case UI_MSG_SHOW_TOAST:
            return "SHOW_TOAST";
        case UI_MSG_SHOW_LOADING:
            return "SHOW_LOADING";
        case UI_MSG_HIDE_LOADING:
            return "HIDE_LOADING";
        case UI_MSG_OTA_UPDATE:
            return "OTA_UPDATE";
        case UI_MSG_SETTINGS_ACTION:
            return "SETTINGS_ACTION";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief 把 Presentation 事件转换为 UI Task 私有消息
 *
 * 本回调运行在默认 ESP Event Loop 上下文，只复制小型载荷并尝试入队。普通事件保持原队列
 * 语义；状态刷新先写持久 pending，再尽力投递 marker，因此业务队列已满时仍会由 UI Task
 * 收敛最新 Presenter View Model。UI 入口关闭期间只合并恢复标记和最后页面，最新业务数据
 * 仍由 Presenter 的 View Model 保存。
 */
static void on_presentation_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;

    ui_msg_t message = { 0 };
    switch ((presentation_event_id_t) id)
    {
        case PRESENTATION_EVENT_PAGE_SWITCH:
            if (data == NULL)
            {
                return;
            }
            message.type  = UI_MSG_SWITCH_PAGE;
            message.page  = ((const presentation_page_switch_event_t *) data)->page;
            message.param = (uint32_t) ((const presentation_page_switch_event_t *) data)->dir;
            break;
        case PRESENTATION_EVENT_STATUS_BAR_UPDATE:
            message.type = UI_MSG_STATUS_BAR_UPDATE;
            break;
        case PRESENTATION_EVENT_STATUS_UPDATE: {
            message.type          = UI_MSG_STATUS_UPDATE;
            const esp_err_t error = post_status_update_marker();
            if (error == ESP_ERR_INVALID_STATE && get_state() != UI_RUNTIME_STATE_FAILED)
            {
                merge_message_for_resume(&message);
                ESP_LOGD(TAG, "UI 暂不可用，Presentation 状态刷新已合并");
                return;
            }
            if (error != ESP_OK)
            {
                ESP_LOGW(TAG, "Presentation 状态刷新转交 UI 失败: err=%s", esp_err_to_name(error));
            }
            return;
        }
        case PRESENTATION_EVENT_POMODORO_UPDATE:
            message.type = UI_MSG_POMODORO_UPDATE;
            break;
        case PRESENTATION_EVENT_OTA_UPDATE:
            message.type = UI_MSG_OTA_UPDATE;
            break;
        case PRESENTATION_EVENT_SETTINGS_ACTION:
            if (data == NULL)
            {
                return;
            }
            message.type  = UI_MSG_SETTINGS_ACTION;
            message.param = (uint32_t) ((const presentation_settings_action_event_t *) data)->action;
            break;
        default:
            ESP_LOGW(TAG, "忽略未知 Presentation 事件: id=%ld", (long) id);
            return;
    }

    const esp_err_t error = post_message(&message, 0);
    if (error == ESP_ERR_INVALID_STATE && get_state() != UI_RUNTIME_STATE_FAILED)
    {
        merge_message_for_resume(&message);
        ESP_LOGD(TAG, "UI 暂不可用，Presentation 事件已合并: id=%ld", (long) id);
        return;
    }
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "Presentation 事件转交 UI 失败: id=%ld err=%s", (long) id, esp_err_to_name(error));
    }
}

/**
 * @brief 把 UI Task 状态转换为稳定日志名称
 */
static const char *state_name(ui_runtime_state_t state)
{
    switch (state)
    {
        case UI_RUNTIME_STATE_UNINIT:
            return "UNINIT";
        case UI_RUNTIME_STATE_STARTING:
            return "STARTING";
        case UI_RUNTIME_STATE_RUNNING:
            return "RUNNING";
        case UI_RUNTIME_STATE_STOPPING:
            return "STOPPING";
        case UI_RUNTIME_STATE_STOPPED:
            return "STOPPED";
        case UI_RUNTIME_STATE_FAILED:
            return "FAILED";
        default:
            return "UNKNOWN";
    }
}

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
 * @brief 获取平台同步接口可使用的剩余毫秒数
 */
static uint32_t remaining_timeout_ms(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0)
    {
        return 0;
    }
    const uint64_t milliseconds = ((uint64_t) remaining_us + 999ULL) / 1000ULL;
    return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t) milliseconds;
}

/**
 * @brief 原子读取唯一 UI Task 状态
 */
static ui_runtime_state_t get_state(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const ui_runtime_state_t state = s_state;
    taskEXIT_CRITICAL(&s_state_lock);
    return state;
}

/**
 * @brief 原子判断产品 UI Task 句柄是否已经退出
 */
static bool task_handle_is_null(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool is_null = s_task == NULL;
    taskEXIT_CRITICAL(&s_state_lock);
    return is_null;
}

/**
 * @brief 原子更新 UI Task 状态和业务入口，并在恢复 RUNNING 时强制刷新最新 View Model
 *
 * 恢复 `RUNNING + business_open` 表示此前关闭入口的停止尝试失败；该窗口中的状态事件可能
 * 已被合并丢弃，因此在同一状态锁内置 pending 并登记一次活跃通知者。Task 句柄在锁内复制，
 * 锁外通知后才撤销登记；若 UI Task 随即处理控制命令，停止收敛会等待该登记结束，不会通知
 * 已删除 Task。成功 STOP、FAILED、STOPPED 和 UNINIT 不创建刷新。
 */
static void set_state(ui_runtime_state_t state, bool business_open)
{
    TaskHandle_t task_to_notify = NULL;
    taskENTER_CRITICAL(&s_state_lock);
    const ui_runtime_state_t old = s_state;
    s_state                      = state;
    s_business_open              = business_open;
    if (state == UI_RUNTIME_STATE_RUNNING && business_open)
    {
        s_status_update_pending = true;
        if (s_task != NULL)
        {
            s_active_posters++;
            task_to_notify = s_task;
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (task_to_notify != NULL)
    {
        xTaskNotifyGive(task_to_notify);
        end_business_post();
    }
    ESP_LOGI(TAG, "状态转换: %s -> %s，业务入口=%d", state_name(old), state_name(state), (int) business_open);
}

/**
 * @brief 仅当 UI Task 仍处于 RUNNING 时恢复控制失败关闭的业务入口
 *
 * 外部停止调用方不得把已经进入 STOPPING/STOPPED 的 UI Task 反向改回 RUNNING。符合条件时
 * 复用 `set_state()` 的 pending、通知和句柄生命周期保护。
 */
static void restore_running_after_control_failure(void)
{
    TaskHandle_t task_to_notify = NULL;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state == UI_RUNTIME_STATE_RUNNING && s_task != NULL)
    {
        s_business_open         = true;
        s_status_update_pending = true;
        s_active_posters++;
        task_to_notify = s_task;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (task_to_notify != NULL)
    {
        xTaskNotifyGive(task_to_notify);
        end_business_post();
        ESP_LOGI(TAG, "控制失败后恢复 RUNNING，已安排完整状态刷新");
    }
}

/**
 * @brief 在业务投递期间登记活跃发送者，供休眠边界等待
 */
static bool begin_business_post(void)
{
    bool accepted = false;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state == UI_RUNTIME_STATE_RUNNING && s_business_open)
    {
        s_active_posters++;
        accepted = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return accepted;
}

/**
 * @brief 结束一次业务消息投递登记
 */
static void end_business_post(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_active_posters > 0)
    {
        s_active_posters--;
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

/**
 * @brief 耐久合并状态刷新并尽力投递普通业务队列 marker
 *
 * 默认 Event Loop 上下文先在状态锁内设置 pending 并复制 Task 句柄，再以零等待尝试入队。
 * marker 入队失败不丢刷新事实；Task notification 保证 UI Task 在再次阻塞前主动收敛 pending。
 * 活跃发送者记账使停止流程不会在本函数访问队列或 Task 句柄期间释放运行期资源。
 *
 * @return ESP_OK pending 已由 UI Runtime 接受；ESP_ERR_INVALID_STATE 业务入口未开放
 */
static esp_err_t post_status_update_marker(void)
{
    if (!begin_business_post())
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_status_update_pending = true;
    TaskHandle_t task       = s_task;
    taskEXIT_CRITICAL(&s_state_lock);

    const ui_msg_t marker = {
        .type = UI_MSG_STATUS_UPDATE,
    };
    const BaseType_t marker_sent = xQueueSend(s_message_queue, &marker, 0);
    if (task != NULL)
    {
        xTaskNotifyGive(task);
    }
    end_business_post();

    if (marker_sent != pdTRUE)
    {
        ESP_LOGD(TAG, "UI 消息队列已满，状态刷新保留为 pending");
    }
    return ESP_OK;
}

/**
 * @brief 读取当前仍在投递业务消息的调用者数量
 */
static uint32_t active_poster_count(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const uint32_t count = s_active_posters;
    taskEXIT_CRITICAL(&s_state_lock);
    return count;
}

/**
 * @brief 在持有 LVGL 锁的上下文串行处理一条业务消息
 */
static void handle_business_message(const ui_msg_t *message)
{
    ESP_LOGI(TAG, "收到 UI 消息: 类型=%s(%u)", message_name(message->type), (unsigned) message->type);
    if (!ui_platform_lvgl_lock(UINT32_MAX))
    {
        ESP_LOGE(TAG, "获取 LVGL 锁失败，丢弃 UI 消息: type=%u", (unsigned) message->type);
        return;
    }

    esp_err_t err = ui_main_handle_message(message);
    ESP_LOGI(TAG,
             "UI 消息处理完成: 类型=%s(%u)，结果=%s",
             message_name(message->type),
             (unsigned) message->type,
             esp_err_to_name(err));
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW(TAG, "处理 UI 消息失败: type=%u err=%s", (unsigned) message->type, esp_err_to_name(err));
    }
    ui_platform_lvgl_unlock();
}

/**
 * @brief 在 UI Task 上下文原子取得一次可合并状态刷新
 *
 * @return true 已取得并清除 pending；false 当前没有可处理刷新
 */
static bool take_status_update_pending(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool pending = s_state == UI_RUNTIME_STATE_RUNNING && s_business_open && s_status_update_pending;
    if (pending)
    {
        s_status_update_pending = false;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return pending;
}

/**
 * @brief 主动收敛最新 View Model；重复队列 marker 不会重复刷新
 *
 * @return true 本次处理了一次 pending；false marker 已过时或当前无 pending
 */
static bool drain_status_update_pending(void)
{
    if (!take_status_update_pending())
    {
        return false;
    }
    const ui_msg_t message = {
        .type = UI_MSG_STATUS_UPDATE,
    };
    handle_business_message(&message);
    return true;
}

/**
 * @brief 等待停止边界内的发送者结束，并丢弃全部已接受消息
 */
static esp_err_t settle_business_messages(int64_t deadline_us)
{
    ui_msg_t message;
    for (;;)
    {
        while (xQueueReceive(s_message_queue, &message, 0) == pdTRUE)
        {
            merge_message_for_resume(&message);
            ESP_LOGW(TAG, "停止期间丢弃 UI 消息: type=%u", (unsigned) message.type);
            if (esp_timer_get_time() >= deadline_us)
            {
                return ESP_ERR_TIMEOUT;
            }
        }
        if (active_poster_count() == 0 && uxQueueMessagesWaiting(s_message_queue) == 0)
        {
            taskENTER_CRITICAL(&s_state_lock);
            s_status_update_pending = false;
            taskEXIT_CRITICAL(&s_state_lock);
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline_us)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

/**
 * @brief 完成固定控制回执并唤醒当前等待者
 */
static void complete_control(uint32_t request_id, esp_err_t result)
{
    taskENTER_CRITICAL(&s_control_lock);
    s_completed_request_id = request_id;
    s_completed_result     = result;
    taskEXIT_CRITICAL(&s_control_lock);
    (void) xSemaphoreGive(s_control_signal);
}

/**
 * @brief 在 UI Task 上下文关闭入口、停止 LVGL/显示活动并保留运行时资源
 */
static esp_err_t handle_stop(const ui_task_control_t *control)
{
    ESP_RETURN_ON_FALSE(get_state() == UI_RUNTIME_STATE_RUNNING, ESP_ERR_INVALID_STATE, TAG, "UI Task 状态不允许停止");
    set_state(UI_RUNTIME_STATE_STOPPING, false);
    const esp_err_t settle_error = settle_business_messages(control->deadline_us);
    if (settle_error != ESP_OK)
    {
        set_state(UI_RUNTIME_STATE_RUNNING, true);
        return settle_error;
    }

    const uint32_t timeout_ms = remaining_timeout_ms(control->deadline_us);
    if (timeout_ms == 0U)
    {
        set_state(UI_RUNTIME_STATE_RUNNING, true);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t stop_error = ui_platform_lvgl_stop(timeout_ms);
    if (stop_error != ESP_OK)
    {
        const uint32_t  rollback_timeout_ms = remaining_timeout_ms(control->deadline_us);
        const esp_err_t rollback_error =
            rollback_timeout_ms == 0U ? ESP_ERR_TIMEOUT : ui_platform_lvgl_start(rollback_timeout_ms);
        if (rollback_error == ESP_OK)
        {
            set_state(UI_RUNTIME_STATE_RUNNING, true);
        }
        else
        {
            ESP_LOGE(TAG,
                     "UI 停止失败且恢复运行态失败: stop=%s resume=%s",
                     esp_err_to_name(stop_error),
                     esp_err_to_name(rollback_error));
            set_state(UI_RUNTIME_STATE_FAILED, false);
        }
        return stop_error;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_resume_refresh_pending = true;
    taskEXIT_CRITICAL(&s_state_lock);
    set_state(UI_RUNTIME_STATE_STOPPED, false);
    return ESP_OK;
}

/**
 * @brief 在 LVGL timer 尚未恢复时把保留控件树同步到最新 Presenter 状态
 */
static esp_err_t resync_retained_ui(int64_t deadline_us)
{
    const ui_resume_snapshot_t snapshot = take_resume_snapshot();

    const uint32_t timeout_ms           = remaining_timeout_ms(deadline_us);
    if (timeout_ms == 0U || !ui_platform_lvgl_lock(timeout_ms))
    {
        restore_resume_snapshot(&snapshot);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t error = ui_main_resync(snapshot.page_switch_valid ? &snapshot.page_switch : NULL);
    ui_platform_lvgl_unlock();
    if (error != ESP_OK)
    {
        restore_resume_snapshot(&snapshot);
    }
    return error;
}

/**
 * @brief 在 UI Task 上下文恢复保留的 LVGL、显示和控件状态
 */
static esp_err_t handle_start(const ui_task_control_t *control)
{
    ESP_RETURN_ON_FALSE(get_state() == UI_RUNTIME_STATE_STARTING && s_runtime_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "UI Task 状态不允许恢复");

    esp_err_t error = resync_retained_ui(control->deadline_us);
    if (error == ESP_OK)
    {
        const uint32_t timeout_ms = remaining_timeout_ms(control->deadline_us);
        error                     = timeout_ms == 0U ? ESP_ERR_TIMEOUT : ui_platform_lvgl_start(timeout_ms);
    }
    if (error != ESP_OK)
    {
        const uint32_t  rollback_timeout_ms = remaining_timeout_ms(control->deadline_us);
        const esp_err_t rollback_error =
            rollback_timeout_ms == 0U ? ESP_ERR_TIMEOUT : ui_platform_lvgl_stop(rollback_timeout_ms);
        if (rollback_error == ESP_OK)
        {
            set_state(UI_RUNTIME_STATE_STOPPED, false);
        }
        else
        {
            ESP_LOGE(TAG,
                     "UI 恢复失败且回滚停止态失败: start=%s stop=%s",
                     esp_err_to_name(error),
                     esp_err_to_name(rollback_error));
            set_state(UI_RUNTIME_STATE_FAILED, false);
        }
        return error;
    }

    ui_msg_t pending_page = { 0 };
    bool     has_page;
    bool     needs_refresh;
    taskENTER_CRITICAL(&s_state_lock);
    has_page                    = s_pending_page_switch_valid;
    pending_page                = s_pending_page_switch;
    needs_refresh               = s_resume_refresh_pending;
    s_pending_page_switch_valid = false;
    s_resume_refresh_pending    = false;
    s_state                     = UI_RUNTIME_STATE_RUNNING;
    s_business_open             = true;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "状态转换: STARTING -> RUNNING，业务入口=1");

    if (has_page)
    {
        handle_business_message(&pending_page);
    }
    else if (needs_refresh)
    {
        const ui_msg_t refresh = {
            .type = UI_MSG_STATUS_UPDATE,
        };
        handle_business_message(&refresh);
    }
    return ESP_OK;
}

/**
 * @brief 在停止态完整释放 UI 运行时；仅成功时允许产品 Task 退出
 */
static esp_err_t handle_deinit(const ui_task_control_t *control)
{
    const ui_runtime_state_t state = get_state();
    ESP_RETURN_ON_FALSE(state == UI_RUNTIME_STATE_STOPPED || state == UI_RUNTIME_STATE_FAILED,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "UI Task 状态不允许反初始化");
    const uint32_t timeout_ms = remaining_timeout_ms(control->deadline_us);
    if (timeout_ms == 0U)
    {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t error = deinitialize_runtime(timeout_ms);
    if (error != ESP_OK)
    {
        set_state(UI_RUNTIME_STATE_FAILED, false);
        return error;
    }
    set_state(UI_RUNTIME_STATE_STOPPED, false);
    return ESP_OK;
}

/**
 * @brief 在退出回执发出前原子清空当前产品 Task 句柄
 */
static void clear_current_task_handle(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    taskEXIT_CRITICAL(&s_state_lock);
}

/**
 * @brief 处理一条独立控制命令并返回是否应退出 Task
 */
static bool handle_control(const ui_task_control_t *control)
{
    esp_err_t result    = ESP_ERR_TIMEOUT;
    bool      exit_task = false;
    if (esp_timer_get_time() < control->deadline_us)
    {
        switch (control->type)
        {
            case UI_TASK_CONTROL_START:
                result = handle_start(control);
                break;
            case UI_TASK_CONTROL_STOP:
                result = handle_stop(control);
                break;
            case UI_TASK_CONTROL_DEINIT:
                result    = handle_deinit(control);
                exit_task = result == ESP_OK;
                break;
            default:
                result = ESP_ERR_INVALID_ARG;
                break;
        }
    }
    if (exit_task)
    {
        clear_current_task_handle();
    }
    complete_control(control->request_id, result);
    return exit_task;
}

/**
 * @brief 按控件树、字体、LVGL 平台的逆序释放完整 UI 运行时
 */
static esp_err_t deinitialize_runtime(uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    esp_err_t     result      = ESP_OK;
    if (s_runtime_initialized)
    {
        if (!ui_platform_lvgl_lock(timeout_ms))
        {
            ESP_LOGE(TAG, "清理 UI 控件树前获取 LVGL 锁超时");
            return ESP_ERR_TIMEOUT;
        }
        else
        {
            result = ui_main_deinit();
            ui_platform_lvgl_unlock();
        }
    }

    ui_platform_font_deinit();
    uint32_t platform_timeout_ms = remaining_timeout_ms(deadline_us);
    if (platform_timeout_ms == 0)
    {
        platform_timeout_ms = 1;
    }
    const esp_err_t platform_err = ui_platform_lvgl_deinit(platform_timeout_ms);
    if (platform_err != ESP_OK)
    {
        ESP_LOGE(TAG, "释放 LVGL 平台失败: %s", esp_err_to_name(platform_err));
        if (result == ESP_OK)
        {
            result = platform_err;
        }
    }
    s_runtime_initialized = false;
    return result;
}

/**
 * @brief 在启动失败或取消后使用固定上限回滚已创建的 UI 资源
 */
static void rollback_runtime(void)
{
    const esp_err_t err = deinitialize_runtime(UI_TASK_ROLLBACK_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "回滚 UI 运行时失败: %s", esp_err_to_name(err));
    }
}

/**
 * @brief 在 UI Task 上下文依次初始化 LVGL、字体和同步控件树
 */
static esp_err_t initialize_runtime(void)
{
    esp_err_t err = ui_platform_lvgl_init();
    if (err != ESP_OK)
    {
        return err;
    }
    err = ui_platform_font_init();
    if (err != ESP_OK)
    {
        rollback_runtime();
        return err;
    }

    const uint32_t timeout_ms = remaining_timeout_ms(s_start_deadline_us);
    if (timeout_ms == 0 || !ui_platform_lvgl_lock(timeout_ms))
    {
        rollback_runtime();
        return ESP_ERR_TIMEOUT;
    }
    err = ui_main_init();
    ui_platform_lvgl_unlock();
    if (err != ESP_OK)
    {
        rollback_runtime();
        return err;
    }

    s_runtime_initialized = true;
    ESP_LOGI(TAG, "UI 运行时初始化完成: 字体状态=%d", (int) ui_platform_font_get_status());
    return ESP_OK;
}

/**
 * @brief 原子完成 READY/FAILED 回执并决定最终启动结果
 */
static esp_err_t complete_startup(esp_err_t result)
{
    taskENTER_CRITICAL(&s_state_lock);
    const ui_runtime_state_t old = s_state;
    if (result == ESP_OK && s_start_cancelled)
    {
        result = ESP_ERR_TIMEOUT;
    }
    s_start_result    = result;
    s_start_completed = true;
    if (result == ESP_OK)
    {
        s_state         = UI_RUNTIME_STATE_RUNNING;
        s_business_open = true;
    }
    else
    {
        s_state         = UI_RUNTIME_STATE_FAILED;
        s_business_open = false;
    }
    const ui_runtime_state_t current = s_state;
    const bool               open    = s_business_open;
    taskEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG,
             "状态转换: %s -> %s，业务入口=%d，结果=%s",
             state_name(old),
             state_name(current),
             (int) open,
             esp_err_to_name(result));
    (void) xSemaphoreGive(s_start_signal);
    return result;
}

/**
 * @brief 清除产品 Task 句柄并删除当前 Task
 */
static void delete_current_task(void)
{
    clear_current_task_handle();
    vTaskDelete(NULL);
}

/**
 * @brief 产品 UI Task 主循环，阻塞前与业务消息之间主动收敛状态刷新 pending
 */
static void ui_runtime_task(void *arg)
{
    (void) arg;

    const esp_err_t init_result = initialize_runtime();
    if (init_result != ESP_OK)
    {
        clear_current_task_handle();
        (void) complete_startup(init_result);
        vTaskDelete(NULL);
        return;
    }

    const esp_err_t start_result = complete_startup(ESP_OK);
    if (start_result != ESP_OK)
    {
        rollback_runtime();
        delete_current_task();
        return;
    }

    ui_task_control_t control;
    ui_msg_t          message;
    for (;;)
    {
        for (;;)
        {
            if (xQueueReceive(s_control_queue, &control, 0) == pdTRUE)
            {
                if (handle_control(&control))
                {
                    goto stopped;
                }
                continue;
            }
            if (drain_status_update_pending())
            {
                continue;
            }
            if (xQueueReceive(s_message_queue, &message, 0) != pdTRUE)
            {
                break;
            }
            if (message.type == UI_MSG_STATUS_UPDATE)
            {
                /*
                 * pending 在每次取队列前已主动收敛；若生产者恰在该窗口置位，
                 * 再次 drain 会处理最新事实，否则此 marker 只是幂等唤醒。
                 */
                (void) drain_status_update_pending();
                continue;
            }
            if (get_state() == UI_RUNTIME_STATE_RUNNING)
            {
                handle_business_message(&message);
            }
            else
            {
                ESP_LOGW(TAG,
                         "非 RUNNING 状态丢弃 UI 消息: state=%s type=%u",
                         state_name(get_state()),
                         (unsigned) message.type);
            }
        }
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

stopped:
    vTaskDelete(NULL);
}

/**
 * @brief 初始化固定启动/控制信号量和回执状态
 */
static esp_err_t initialize_control_response(void)
{
    if (s_control_mutex == NULL)
    {
        s_control_mutex = xSemaphoreCreateMutexStatic(&s_control_mutex_storage);
    }
    if (s_control_signal == NULL)
    {
        s_control_signal = xSemaphoreCreateBinaryStatic(&s_control_signal_storage);
    }
    if (s_start_signal == NULL)
    {
        s_start_signal = xSemaphoreCreateBinaryStatic(&s_start_signal_storage);
    }
    ESP_RETURN_ON_FALSE(s_control_mutex != NULL && s_control_signal != NULL && s_start_signal != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "创建 UI 启动/控制回执失败");
    (void) xSemaphoreTake(s_control_signal, 0);
    (void) xSemaphoreTake(s_start_signal, 0);
    return ESP_OK;
}

/**
 * @brief 创建产品 UI 业务/控制队列和唯一状态机
 */
esp_err_t ui_runtime_init(void)
{
    if (get_state() != UI_RUNTIME_STATE_UNINIT)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(initialize_control_response(), TAG, "初始化 UI 控制回执失败");

    s_message_queue = xQueueCreate(UI_TASK_MESSAGE_QUEUE_LEN, sizeof(ui_msg_t));
    ESP_RETURN_ON_FALSE(s_message_queue != NULL, ESP_ERR_NO_MEM, TAG, "创建 UI 消息队列失败");
    s_control_queue = xQueueCreate(UI_TASK_CONTROL_QUEUE_LEN, sizeof(ui_task_control_t));
    if (s_control_queue == NULL)
    {
        vQueueDelete(s_message_queue);
        s_message_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t register_error =
        esp_event_handler_register(PRESENTATION_EVENT, ESP_EVENT_ANY_ID, on_presentation_event, NULL);
    if (register_error != ESP_OK)
    {
        vQueueDelete(s_control_queue);
        vQueueDelete(s_message_queue);
        s_control_queue = NULL;
        s_message_queue = NULL;
        return register_error;
    }
    s_presentation_handler_registered = true;
    set_state(UI_RUNTIME_STATE_STOPPED, false);
    return ESP_OK;
}

esp_err_t ui_runtime_set_user_intent_callback_borrow(ui_user_intent_callback_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(callback != NULL || context == NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "清除 UI 用户意图回调时上下文必须为空");
    ESP_RETURN_ON_FALSE(get_state() == UI_RUNTIME_STATE_STOPPED,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "UI Runtime 当前状态不允许修改用户意图回调");

    taskENTER_CRITICAL(&s_state_lock);
    s_user_intent_callback = callback;
    s_user_intent_context  = context;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t ui_runtime_emit_user_intent(const ui_user_intent_t *intent, ui_user_intent_result_t *out_result)
{
    ESP_RETURN_ON_FALSE(intent != NULL && (unsigned) intent->id < UI_USER_INTENT_COUNT,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "UI 用户意图无效");
    ESP_RETURN_ON_FALSE(intent->id != UI_USER_INTENT_POMODORO_SETTINGS_SAVE || out_result != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "番茄钟设置意图缺少请求结果输出");
    if (intent->id == UI_USER_INTENT_SCREEN_LOADED)
    {
        ESP_RETURN_ON_FALSE((unsigned) intent->page < PRESENTATION_PAGE_COUNT,
                            ESP_ERR_INVALID_ARG,
                            TAG,
                            "Screen 加载完成页面无效");
    }

    taskENTER_CRITICAL(&s_state_lock);
    ui_user_intent_callback_t callback = s_user_intent_callback;
    void                     *context  = s_user_intent_context;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_STATE, TAG, "UI 用户意图回调尚未注册");
    if (out_result != NULL)
    {
        *out_result = (ui_user_intent_result_t) { 0 };
    }
    return callback(intent, out_result, context);
}

/**
 * @brief 创建产品 UI Task 并等待平台、字体和控件树 READY
 */
esp_err_t ui_runtime_start(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "UI 启动超时无效");
    const ui_runtime_state_t state = get_state();
    if (state == UI_RUNTIME_STATE_RUNNING)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(state == UI_RUNTIME_STATE_STOPPED, ESP_ERR_INVALID_STATE, TAG, "UI Task 状态不允许启动");
    ESP_RETURN_ON_FALSE(s_message_queue != NULL && s_control_queue != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "UI Task 生命周期资源尚未初始化");
    if (!task_handle_is_null())
    {
        return request_control(UI_TASK_CONTROL_START, timeout_ms);
    }

    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    (void) xSemaphoreTake(s_start_signal, 0);
    taskENTER_CRITICAL(&s_state_lock);
    const ui_runtime_state_t old = s_state;
    s_state                      = UI_RUNTIME_STATE_STARTING;
    s_business_open              = false;
    s_status_update_pending      = false;
    s_start_deadline_us          = deadline_us;
    s_start_cancelled            = false;
    s_start_completed            = false;
    s_start_result               = ESP_FAIL;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG, "状态转换: %s -> STARTING，业务入口=0，超时=%lu ms", state_name(old), (unsigned long) timeout_ms);

    /* 字体分区 mmap/munmap 会冻结 Cache；当前 Task 的栈必须位于内部 SRAM，
     * 否则 ESP-IDF 会在冻结 Cache 前触发栈安全断言。 */
    const BaseType_t ok = xTaskCreateWithCaps(ui_runtime_task,
                                              "ui_task",
                                              UI_TASK_STACK,
                                              NULL,
                                              UI_TASK_PRIORITY,
                                              &s_task,
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS)
    {
        set_state(UI_RUNTIME_STATE_STOPPED, false);
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_start_signal, ticks_until(deadline_us)) != pdTRUE)
    {
        taskENTER_CRITICAL(&s_state_lock);
        if (!s_start_completed)
        {
            s_start_cancelled = true;
            s_start_result    = ESP_ERR_TIMEOUT;
        }
        const esp_err_t result = s_start_result;
        taskEXIT_CRITICAL(&s_state_lock);
        return result;
    }
    taskENTER_CRITICAL(&s_state_lock);
    const esp_err_t result = s_start_result;
    taskEXIT_CRITICAL(&s_state_lock);
    return result;
}

/**
 * @brief 向产品 UI Task 按值投递一条业务消息
 */
static esp_err_t post_message(const ui_msg_t *message, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(message != NULL, ESP_ERR_INVALID_ARG, TAG, "UI 消息为空");
    if (!begin_business_post())
    {
        ESP_LOGD(TAG,
                 "UI 业务入口未开放，拒绝消息: state=%s type=%s(%u)",
                 state_name(get_state()),
                 message_name(message->type),
                 (unsigned) message->type);
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    const BaseType_t sent  = xQueueSend(s_message_queue, message, ticks);
    if (sent != pdTRUE)
    {
        end_business_post();
        ESP_LOGW(TAG, "UI 消息队列已满: type=%u", (unsigned) message->type);
        return ESP_ERR_TIMEOUT;
    }
    xTaskNotifyGive(s_task);
    end_business_post();
    ESP_LOGI(TAG, "发送 UI 消息: 类型=%s(%u)", message_name(message->type), (unsigned) message->type);
    return ESP_OK;
}

/**
 * @brief 通过独立控制 Queue 和固定回执执行一条有界生命周期命令
 */
static esp_err_t request_control(ui_task_control_type_t type, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "UI 控制超时无效");
    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_control_mutex, ticks_until(deadline_us)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "等待 UI 控制通道超时");

    bool valid = false;
    taskENTER_CRITICAL(&s_state_lock);
    switch (type)
    {
        case UI_TASK_CONTROL_START:
            valid = s_state == UI_RUNTIME_STATE_STOPPED && s_runtime_initialized && s_task != NULL;
            if (valid)
            {
                s_state         = UI_RUNTIME_STATE_STARTING;
                s_business_open = false;
            }
            break;
        case UI_TASK_CONTROL_STOP:
            valid = s_state == UI_RUNTIME_STATE_RUNNING && s_task != NULL;
            if (valid)
            {
                s_business_open = false;
            }
            break;
        case UI_TASK_CONTROL_DEINIT:
            valid = (s_state == UI_RUNTIME_STATE_STOPPED || s_state == UI_RUNTIME_STATE_FAILED) && s_task != NULL;
            break;
        default:
            break;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!valid)
    {
        (void) xSemaphoreGive(s_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_control_lock);
    s_next_request_id++;
    if (s_next_request_id == 0)
    {
        s_next_request_id++;
    }
    const uint32_t request_id = s_next_request_id;
    taskEXIT_CRITICAL(&s_control_lock);
    (void) xSemaphoreTake(s_control_signal, 0);

    const ui_task_control_t control = {
        .type        = type,
        .request_id  = request_id,
        .deadline_us = deadline_us,
    };
    if (xQueueSend(s_control_queue, &control, ticks_until(deadline_us)) != pdTRUE)
    {
        if (type == UI_TASK_CONTROL_STOP && get_state() == UI_RUNTIME_STATE_RUNNING)
        {
            restore_running_after_control_failure();
        }
        else if (type == UI_TASK_CONTROL_START && get_state() == UI_RUNTIME_STATE_STARTING)
        {
            set_state(UI_RUNTIME_STATE_STOPPED, false);
        }
        (void) xSemaphoreGive(s_control_mutex);
        return ESP_ERR_TIMEOUT;
    }
    xTaskNotifyGive(s_task);

    esp_err_t result = ESP_ERR_TIMEOUT;
    for (;;)
    {
        taskENTER_CRITICAL(&s_control_lock);
        const bool completed = s_completed_request_id == request_id;
        if (completed)
        {
            result = s_completed_result;
        }
        taskEXIT_CRITICAL(&s_control_lock);
        if (completed)
        {
            break;
        }
        const TickType_t wait_ticks = ticks_until(deadline_us);
        if (wait_ticks == 0 || xSemaphoreTake(s_control_signal, wait_ticks) != pdTRUE)
        {
            break;
        }
    }
    if (result != ESP_OK)
    {
        restore_running_after_control_failure();
    }
    (void) xSemaphoreGive(s_control_mutex);
    return result;
}

/**
 * @brief 同步关闭业务入口并把产品 UI Task 留在可恢复停止态
 */
esp_err_t ui_runtime_stop(uint32_t timeout_ms)
{
    if (get_state() == UI_RUNTIME_STATE_STOPPED)
    {
        return ESP_OK;
    }
    return request_control(UI_TASK_CONTROL_STOP, timeout_ms);
}

/**
 * @brief 让停止态 UI Task 完整释放运行时，再删除生命周期队列并恢复初始状态
 */
esp_err_t ui_runtime_deinit(void)
{
    const ui_runtime_state_t state = get_state();
    if (state == UI_RUNTIME_STATE_UNINIT)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(state == UI_RUNTIME_STATE_STOPPED || state == UI_RUNTIME_STATE_FAILED,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "UI Task 状态不允许反初始化");
    if (!task_handle_is_null())
    {
        ESP_RETURN_ON_ERROR(request_control(UI_TASK_CONTROL_DEINIT, UI_TASK_ROLLBACK_TIMEOUT_MS),
                            TAG,
                            "UI Task 反初始化失败");
    }
    ESP_RETURN_ON_FALSE(task_handle_is_null(), ESP_ERR_INVALID_STATE, TAG, "UI Task 尚未退出");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_control_mutex, 0) == pdTRUE,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "UI 控制通道仍在使用");

    if (s_presentation_handler_registered)
    {
        const esp_err_t unregister_error =
            esp_event_handler_unregister(PRESENTATION_EVENT, ESP_EVENT_ANY_ID, on_presentation_event);
        if (unregister_error != ESP_OK)
        {
            (void) xSemaphoreGive(s_control_mutex);
            return unregister_error;
        }
        s_presentation_handler_registered = false;
    }

    if (s_message_queue != NULL)
    {
        vQueueDelete(s_message_queue);
        s_message_queue = NULL;
    }
    if (s_control_queue != NULL)
    {
        vQueueDelete(s_control_queue);
        s_control_queue = NULL;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_active_posters            = 0;
    s_business_open             = false;
    s_status_update_pending     = false;
    s_start_deadline_us         = 0;
    s_start_cancelled           = false;
    s_start_completed           = false;
    s_start_result              = ESP_FAIL;
    s_resume_refresh_pending    = false;
    s_pending_page_switch_valid = false;
    s_pending_page_switch       = (ui_msg_t) { 0 };
    s_user_intent_callback      = NULL;
    s_user_intent_context       = NULL;
    taskEXIT_CRITICAL(&s_state_lock);
    taskENTER_CRITICAL(&s_control_lock);
    s_next_request_id      = 0;
    s_completed_request_id = 0;
    s_completed_result     = ESP_FAIL;
    taskEXIT_CRITICAL(&s_control_lock);
    (void) xSemaphoreTake(s_control_signal, 0);
    (void) xSemaphoreTake(s_start_signal, 0);
    set_state(UI_RUNTIME_STATE_UNINIT, false);
    (void) xSemaphoreGive(s_control_mutex);
    return ESP_OK;
}

/**
 * @brief 获取唯一 UI Task 状态机快照
 */
ui_runtime_state_t ui_runtime_get_state(void)
{
    return get_state();
}
