/**
 * @file app_pomodoro.c
 * @brief 管理番茄钟生命周期资源并提供线程安全公共命令入口
 */
#include "app_pomodoro_internal.h"

#include <limits.h>

#include "app_page.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "system_clock.h"

#define APP_POMODORO_QUEUE_LENGTH     12U
#define APP_POMODORO_TASK_STACK_SIZE  5120U
#define APP_POMODORO_TASK_PRIORITY    3U
#define APP_POMODORO_START_TIMEOUT_MS 2000U

static const char *TAG                        = "app_pomodoro";

app_pomodoro_runtime_t g_app_pomodoro_runtime = {
    .timer_lock = portMUX_INITIALIZER_UNLOCKED,
};

static const app_pomodoro_settings_t DEFAULT_SETTINGS = {
    .focus_minutes       = 25U,
    .short_break_minutes = 5U,
    .long_break_minutes  = 15U,
    .long_break_interval = 4U,
};

bool app_pomodoro_settings_are_valid(const app_pomodoro_settings_t *settings)
{
    return settings != NULL && settings->focus_minutes >= 5U && settings->focus_minutes <= 90U
           && (settings->focus_minutes % 5U) == 0U && settings->short_break_minutes >= 1U
           && settings->short_break_minutes <= 30U && settings->long_break_minutes >= 5U
           && settings->long_break_minutes <= 60U && (settings->long_break_minutes % 5U) == 0U
           && settings->long_break_interval >= 2U && settings->long_break_interval <= 8U;
}

/** @brief 阶段 one-shot Timer 回调只投递携带代次的 TICK 命令 */
static void phase_timer_callback(void *arg)
{
    (void) arg;
    taskENTER_CRITICAL(&g_app_pomodoro_runtime.timer_lock);
    const uint64_t generation = g_app_pomodoro_runtime.scheduled_generation;
    taskEXIT_CRITICAL(&g_app_pomodoro_runtime.timer_lock);
    const app_pomodoro_command_t command = {
        .type       = APP_POMODORO_COMMAND_TICK,
        .generation = generation,
    };
    if (g_app_pomodoro_runtime.queue != NULL && xQueueSend(g_app_pomodoro_runtime.queue, &command, 0) != pdTRUE)
    {
        ESP_EARLY_LOGW(TAG, "番茄钟节拍队列已满");
    }
}

/** @brief 午夜 one-shot Timer 回调只投递日期归一化命令 */
static void date_timer_callback(void *arg)
{
    (void) arg;
    const app_pomodoro_command_t command = {
        .type = APP_POMODORO_COMMAND_NORMALIZE_DATE,
    };
    if (g_app_pomodoro_runtime.queue != NULL && xQueueSend(g_app_pomodoro_runtime.queue, &command, 0) != pdTRUE)
    {
        ESP_EARLY_LOGW(TAG, "番茄钟日期队列已满");
    }
}

/** @brief 可信系统时间更新时通知业务 Task 重算日期和午夜 Timer */
static void on_system_clock_updated(system_clock_event_t event, const system_clock_snapshot_t *snapshot, void *ctx)
{
    (void) event;
    (void) snapshot;
    (void) ctx;
    const app_pomodoro_command_t command = {
        .type = APP_POMODORO_COMMAND_NORMALIZE_DATE,
    };
    if (g_app_pomodoro_runtime.running && !g_app_pomodoro_runtime.stopping && g_app_pomodoro_runtime.queue != NULL)
    {
        (void) xQueueSend(g_app_pomodoro_runtime.queue, &command, 0);
    }
}

/** @brief 删除当前已创建的生命周期资源 */
static void delete_resources(void)
{
    if (g_app_pomodoro_runtime.phase_timer != NULL)
    {
        (void) esp_timer_delete(g_app_pomodoro_runtime.phase_timer);
    }
    if (g_app_pomodoro_runtime.date_timer != NULL)
    {
        (void) esp_timer_delete(g_app_pomodoro_runtime.date_timer);
    }
    if (g_app_pomodoro_runtime.reconcile_sem != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro_runtime.reconcile_sem);
    }
    if (g_app_pomodoro_runtime.reconcile_lock != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro_runtime.reconcile_lock);
    }
    if (g_app_pomodoro_runtime.stopped_sem != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro_runtime.stopped_sem);
    }
    if (g_app_pomodoro_runtime.ready_sem != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro_runtime.ready_sem);
    }
    if (g_app_pomodoro_runtime.state_lock != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro_runtime.state_lock);
    }
    if (g_app_pomodoro_runtime.queue != NULL)
    {
        vQueueDelete(g_app_pomodoro_runtime.queue);
    }
    const app_pomodoro_runtime_t empty = {
        .timer_lock = portMUX_INITIALIZER_UNLOCKED,
    };
    g_app_pomodoro_runtime = empty;
}

esp_err_t app_pomodoro_init(void)
{
    if (g_app_pomodoro_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = pomodoro_store_init();
    if (error != ESP_OK)
    {
        return error;
    }
    pomodoro_store_snapshot_t stored = { 0 };
    error                            = pomodoro_store_load_copy(&stored);
    if (error != ESP_OK)
    {
        return error;
    }

    g_app_pomodoro_runtime.queue          = xQueueCreate(APP_POMODORO_QUEUE_LENGTH, sizeof(app_pomodoro_command_t));
    g_app_pomodoro_runtime.state_lock     = xSemaphoreCreateMutex();
    g_app_pomodoro_runtime.ready_sem      = xSemaphoreCreateBinary();
    g_app_pomodoro_runtime.stopped_sem    = xSemaphoreCreateBinary();
    g_app_pomodoro_runtime.reconcile_lock = xSemaphoreCreateMutex();
    g_app_pomodoro_runtime.reconcile_sem  = xSemaphoreCreateBinary();
    if (g_app_pomodoro_runtime.queue == NULL || g_app_pomodoro_runtime.state_lock == NULL
        || g_app_pomodoro_runtime.ready_sem == NULL || g_app_pomodoro_runtime.stopped_sem == NULL
        || g_app_pomodoro_runtime.reconcile_lock == NULL || g_app_pomodoro_runtime.reconcile_sem == NULL)
    {
        delete_resources();
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t phase_timer_args = {
        .callback = phase_timer_callback,
        .name     = "pomodoro_phase",
    };
    error = esp_timer_create(&phase_timer_args, &g_app_pomodoro_runtime.phase_timer);
    if (error != ESP_OK)
    {
        delete_resources();
        return error;
    }
    const esp_timer_create_args_t date_timer_args = {
        .callback = date_timer_callback,
        .name     = "pomodoro_date",
    };
    error = esp_timer_create(&date_timer_args, &g_app_pomodoro_runtime.date_timer);
    if (error != ESP_OK)
    {
        delete_resources();
        return error;
    }

    app_pomodoro_runtime_data_t initial = { 0 };
    initial.snapshot.settings = stored.settings_valid
                                  ? (app_pomodoro_settings_t) {
                                        .focus_minutes       = stored.settings.focus_minutes,
                                        .short_break_minutes = stored.settings.short_break_minutes,
                                        .long_break_minutes  = stored.settings.long_break_minutes,
                                        .long_break_interval = stored.settings.long_break_interval,
                                    }
                                  : DEFAULT_SETTINGS;
    initial.snapshot.settings_version       = 1U;
    initial.snapshot.phase                  = APP_POMODORO_PHASE_NONE;
    initial.snapshot.next_phase             = APP_POMODORO_PHASE_FOCUS;
    initial.snapshot.run_state              = APP_POMODORO_RUN_STATE_IDLE;
    initial.snapshot.remaining_seconds      = (uint32_t) initial.snapshot.settings.focus_minutes * 60U;
    initial.snapshot.phase_duration_seconds = initial.snapshot.remaining_seconds;
    initial.snapshot.settings_saved         = stored.schema_valid && stored.settings_valid && stored.counts_valid;
    initial.snapshot.generation             = 1U;
    if (stored.schema_valid && stored.counts_valid)
    {
        initial.today_date                   = stored.today_date;
        initial.snapshot.today_focus_count   = stored.today_count;
        initial.snapshot.pending_focus_count = stored.pending_count;
    }

    if (!stored.settings_valid)
    {
        ESP_LOGW(TAG, "番茄钟持久化设置缺失或无效，恢复默认值");
        const pomodoro_store_settings_t defaults = {
            .focus_minutes       = DEFAULT_SETTINGS.focus_minutes,
            .short_break_minutes = DEFAULT_SETTINGS.short_break_minutes,
            .long_break_minutes  = DEFAULT_SETTINGS.long_break_minutes,
            .long_break_interval = DEFAULT_SETTINGS.long_break_interval,
        };
        error                           = pomodoro_store_save_settings_copy(&defaults);
        initial.snapshot.settings_saved = error == ESP_OK;
        initial.snapshot.last_error     = error;
        if (error != ESP_OK)
        {
            ESP_LOGW(TAG, "恢复默认番茄钟设置失败: %s", esp_err_to_name(error));
        }
    }
    if (stored.schema_valid && !stored.counts_valid)
    {
        ESP_LOGW(TAG, "番茄钟完成计数字段无效，已从零恢复");
    }

    g_app_pomodoro_runtime.runtime_data = initial;
    g_app_pomodoro_runtime.initialized  = true;
    error                               = system_clock_register_callback_borrow(on_system_clock_updated, NULL);
    if (error != ESP_OK)
    {
        delete_resources();
        return error;
    }
    ESP_LOGI(TAG,
             "番茄钟已初始化，专注=%u 分钟，长休间隔=%u 轮",
             (unsigned) initial.snapshot.settings.focus_minutes,
             (unsigned) initial.snapshot.settings.long_break_interval);
    return ESP_OK;
}

esp_err_t app_pomodoro_start(void)
{
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.running || g_app_pomodoro_runtime.task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(g_app_pomodoro_runtime.ready_sem, 0) == pdTRUE)
    {
    }
    while (xSemaphoreTake(g_app_pomodoro_runtime.stopped_sem, 0) == pdTRUE)
    {
    }
    g_app_pomodoro_runtime.running  = true;
    g_app_pomodoro_runtime.stopping = false;
    if (xTaskCreate(app_pomodoro_task,
                    "app_pomodoro_task",
                    APP_POMODORO_TASK_STACK_SIZE,
                    NULL,
                    APP_POMODORO_TASK_PRIORITY,
                    &g_app_pomodoro_runtime.task)
        != pdPASS)
    {
        g_app_pomodoro_runtime.running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(g_app_pomodoro_runtime.ready_sem, pdMS_TO_TICKS(APP_POMODORO_START_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_pomodoro_stop(uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.state_lock == NULL
        || g_app_pomodoro_runtime.queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    bool             enqueue_stop;
    xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
    if (!g_app_pomodoro_runtime.running)
    {
        xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    enqueue_stop = !g_app_pomodoro_runtime.stopping;
    if (enqueue_stop)
    {
        g_app_pomodoro_runtime.stopping = true;
    }
    xSemaphoreGive(g_app_pomodoro_runtime.state_lock);

    if (enqueue_stop)
    {
        const TickType_t enqueue_elapsed   = xTaskGetTickCount() - started;
        const TickType_t enqueue_remaining = enqueue_elapsed < timeout ? timeout - enqueue_elapsed : 0U;
        const app_pomodoro_command_t command = { .type = APP_POMODORO_COMMAND_STOP };
        if (xQueueSend(g_app_pomodoro_runtime.queue, &command, enqueue_remaining) != pdTRUE)
        {
            xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
            if (g_app_pomodoro_runtime.running)
            {
                g_app_pomodoro_runtime.stopping = false;
            }
            xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
            return ESP_ERR_TIMEOUT;
        }
    }
    const TickType_t elapsed   = xTaskGetTickCount() - started;
    const TickType_t remaining = elapsed < timeout ? timeout - elapsed : 0U;
    if (xSemaphoreTake(g_app_pomodoro_runtime.stopped_sem, remaining) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
    g_app_pomodoro_runtime.running  = false;
    g_app_pomodoro_runtime.stopping = false;
    xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
    (void) xQueueReset(g_app_pomodoro_runtime.queue);
    return ESP_OK;
}

esp_err_t app_pomodoro_deinit(void)
{
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.running || g_app_pomodoro_runtime.task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = system_clock_unregister_callback(on_system_clock_updated, NULL);
    if (error != ESP_OK)
    {
        return error;
    }
    delete_resources();
    return ESP_OK;
}

/**
 * @brief 在状态锁内校验生命周期并以零等待方式复制一条公共用户命令
 *
 * STOP 会先在同一把锁内关闭此入口，因此成功返回的用户命令一定先于 STOP 接受。
 *
 * @param[in] command 调用期间借用的完整命令
 * @return ESP_OK 已复制入队；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 未运行或正在停止；
 *         ESP_ERR_TIMEOUT 队列已满
 */
static esp_err_t enqueue_command(const app_pomodoro_command_t *command)
{
    if (command == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.state_lock == NULL
        || g_app_pomodoro_runtime.queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
    const esp_err_t result = !g_app_pomodoro_runtime.running || g_app_pomodoro_runtime.stopping
                                 ? ESP_ERR_INVALID_STATE
                                 : (xQueueSend(g_app_pomodoro_runtime.queue, command, 0) == pdTRUE ? ESP_OK
                                                                                                  : ESP_ERR_TIMEOUT);
    xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
    return result;
}

#define DEFINE_SIMPLE_REQUEST(function_name, command_type)               \
    esp_err_t function_name(void)                                        \
    {                                                                    \
        const app_pomodoro_command_t command = { .type = command_type }; \
        return enqueue_command(&command);                                \
    }

DEFINE_SIMPLE_REQUEST(app_pomodoro_request_start, APP_POMODORO_COMMAND_START)
DEFINE_SIMPLE_REQUEST(app_pomodoro_request_toggle_pause, APP_POMODORO_COMMAND_TOGGLE_PAUSE)
DEFINE_SIMPLE_REQUEST(app_pomodoro_request_skip, APP_POMODORO_COMMAND_SKIP)
DEFINE_SIMPLE_REQUEST(app_pomodoro_request_confirm, APP_POMODORO_COMMAND_CONFIRM)
DEFINE_SIMPLE_REQUEST(app_pomodoro_request_reset, APP_POMODORO_COMMAND_RESET)

/**
 * @brief 在状态锁内校验版本、运行状态、IDLE、单 pending 和版本/请求 ID 容量
 *
 * @param[in] update 已通过范围校验的完整更新
 * @return ESP_OK 可以接受；其他值表示当前拒绝原因
 */
static esp_err_t validate_settings_update_locked(const app_pomodoro_settings_update_t *update)
{
    if (!g_app_pomodoro_runtime.running || g_app_pomodoro_runtime.stopping)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const app_pomodoro_snapshot_t *snapshot = &g_app_pomodoro_runtime.runtime_data.snapshot;
    if (update->expected_version != snapshot->settings_version)
    {
        return ESP_ERR_INVALID_VERSION;
    }
    if (snapshot->run_state != APP_POMODORO_RUN_STATE_IDLE || snapshot->settings_version == UINT64_MAX
        || g_app_pomodoro_runtime.next_settings_request_id == UINT64_MAX
        || (g_app_pomodoro_runtime.latest_settings_update_result_valid
            && g_app_pomodoro_runtime.latest_settings_update_result.state
                   == APP_POMODORO_SETTINGS_UPDATE_STATE_PENDING))
    {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t app_pomodoro_validate_settings_update(const app_pomodoro_settings_update_t *update)
{
    if (update == NULL || !app_pomodoro_settings_are_valid(&update->settings))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.state_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
    const esp_err_t result = validate_settings_update_locked(update);
    xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
    return result;
}

esp_err_t app_pomodoro_request_update_settings_copy(
    const app_pomodoro_settings_update_t *update,
    uint64_t *out_request_id)
{
    if (update == NULL || out_request_id == NULL || !app_pomodoro_settings_are_valid(&update->settings))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_request_id = 0U;
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.state_lock == NULL
        || g_app_pomodoro_runtime.queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
    const esp_err_t result = validate_settings_update_locked(update);
    if (result != ESP_OK)
    {
        xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
        return result;
    }

    const uint64_t request_id = g_app_pomodoro_runtime.next_settings_request_id + 1U;
    const app_pomodoro_command_t command = {
        .type                = APP_POMODORO_COMMAND_UPDATE_SETTINGS,
        .settings_update     = *update,
        .settings_request_id = request_id,
    };
    if (xQueueSend(g_app_pomodoro_runtime.queue, &command, 0) != pdTRUE)
    {
        xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
        return ESP_ERR_TIMEOUT;
    }

    g_app_pomodoro_runtime.next_settings_request_id          = request_id;
    g_app_pomodoro_runtime.latest_settings_request_id        = request_id;
    g_app_pomodoro_runtime.latest_settings_update_result     = (app_pomodoro_settings_update_result_t) {
        .state   = APP_POMODORO_SETTINGS_UPDATE_STATE_PENDING,
        .version = g_app_pomodoro_runtime.runtime_data.snapshot.settings_version,
        .error   = ESP_OK,
    };
    g_app_pomodoro_runtime.latest_settings_update_result_valid = true;
    *out_request_id                                             = request_id;
    xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
    return ESP_OK;
}

esp_err_t app_pomodoro_get_settings_update_result_copy(
    uint64_t request_id,
    app_pomodoro_settings_update_result_t *out_result)
{
    if (request_id == 0U || out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.state_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
    if (!g_app_pomodoro_runtime.latest_settings_update_result_valid
        || g_app_pomodoro_runtime.latest_settings_request_id != request_id)
    {
        xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out_result = g_app_pomodoro_runtime.latest_settings_update_result;
    xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
    return ESP_OK;
}

esp_err_t app_pomodoro_get_snapshot_copy(app_pomodoro_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.state_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
    *out_snapshot = g_app_pomodoro_runtime.runtime_data.snapshot;
    if (out_snapshot->run_state == APP_POMODORO_RUN_STATE_RUNNING)
    {
        const int64_t remaining_us      = g_app_pomodoro_runtime.runtime_data.phase_deadline_us - esp_timer_get_time();
        out_snapshot->remaining_seconds = remaining_us > 0 ? (uint32_t) ((remaining_us + 999999LL) / 1000000LL) : 0U;
    }
    xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
    return ESP_OK;
}

bool app_pomodoro_requires_live_display(void)
{
    app_pomodoro_snapshot_t snapshot = { 0 };
    return app_pomodoro_get_snapshot_copy(&snapshot) == ESP_OK && snapshot.run_state == APP_POMODORO_RUN_STATE_RUNNING
           && app_page_get_current() == PRESENTATION_PAGE_POMODORO;
}

esp_err_t app_pomodoro_get_next_wakeup_interval_ms(uint32_t *out_interval_ms)
{
    if (out_interval_ms == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.state_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_app_pomodoro_runtime.state_lock, portMAX_DELAY);
    if (g_app_pomodoro_runtime.runtime_data.snapshot.run_state != APP_POMODORO_RUN_STATE_RUNNING)
    {
        xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const int64_t remaining_us = g_app_pomodoro_runtime.runtime_data.phase_deadline_us - esp_timer_get_time();
    xSemaphoreGive(g_app_pomodoro_runtime.state_lock);
    if (remaining_us <= 0)
    {
        *out_interval_ms = 1U;
    }
    else
    {
        const uint64_t remaining_ms = ((uint64_t) remaining_us + 999ULL) / 1000ULL;
        *out_interval_ms            = remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t) remaining_ms;
        if (*out_interval_ms == 0U)
        {
            *out_interval_ms = 1U;
        }
    }
    return ESP_OK;
}

esp_err_t app_pomodoro_reconcile_after_wakeup(uint32_t timeout_ms, app_pomodoro_wakeup_result_t *out_result)
{
    if (timeout_ms == 0U || out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro_runtime.initialized || g_app_pomodoro_runtime.reconcile_lock == NULL
        || g_app_pomodoro_runtime.reconcile_sem == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(g_app_pomodoro_runtime.reconcile_lock, timeout) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    while (xSemaphoreTake(g_app_pomodoro_runtime.reconcile_sem, 0) == pdTRUE)
    {
    }
    uint32_t request_id = ++g_app_pomodoro_runtime.next_reconcile_request_id;
    if (request_id == 0U)
    {
        request_id = ++g_app_pomodoro_runtime.next_reconcile_request_id;
    }
    const app_pomodoro_command_t command = {
        .type                 = APP_POMODORO_COMMAND_RECONCILE,
        .reconcile_request_id = request_id,
    };
    const TickType_t started = xTaskGetTickCount();
    const esp_err_t enqueue_error = enqueue_command(&command);
    if (enqueue_error != ESP_OK)
    {
        xSemaphoreGive(g_app_pomodoro_runtime.reconcile_lock);
        return enqueue_error;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    for (;;)
    {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout || xSemaphoreTake(g_app_pomodoro_runtime.reconcile_sem, timeout - elapsed) != pdTRUE)
        {
            break;
        }
        if (g_app_pomodoro_runtime.completed_reconcile_request_id == request_id)
        {
            *out_result = g_app_pomodoro_runtime.reconcile_result;
            result      = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(g_app_pomodoro_runtime.reconcile_lock);
    return result;
}

bool app_pomodoro_consume_input(device_button_event_t key_event)
{
    if (key_event == DEVICE_BUTTON_EVENT_LEFT_SHORT || key_event == DEVICE_BUTTON_EVENT_RIGHT_SHORT)
    {
        return false;
    }
    app_pomodoro_snapshot_t snapshot;
    if (app_pomodoro_get_snapshot_copy(&snapshot) != ESP_OK)
    {
        return false;
    }
    if (key_event == DEVICE_BUTTON_EVENT_LEFT_LONG)
    {
        esp_err_t   request_error;
        const char *action;
        switch (snapshot.run_state)
        {
            case APP_POMODORO_RUN_STATE_IDLE:
                action        = "开始专注";
                request_error = app_pomodoro_request_start();
                break;
            case APP_POMODORO_RUN_STATE_RUNNING:
            case APP_POMODORO_RUN_STATE_PAUSED:
                action        = "切换暂停";
                request_error = app_pomodoro_request_toggle_pause();
                break;
            case APP_POMODORO_RUN_STATE_DONE:
                action        = "重置番茄钟";
                request_error = app_pomodoro_request_reset();
                break;
            default:
                return false;
        }
        if (request_error != ESP_OK)
        {
            ESP_LOGW(TAG, "%s请求未被接受: %s", action, esp_err_to_name(request_error));
        }
        return true;
    }
    if (key_event == DEVICE_BUTTON_EVENT_RIGHT_LONG)
    {
        esp_err_t   request_error;
        const char *action;
        switch (snapshot.run_state)
        {
            case APP_POMODORO_RUN_STATE_RUNNING:
                action        = "跳过当前阶段";
                request_error = app_pomodoro_request_skip();
                break;
            case APP_POMODORO_RUN_STATE_PAUSED:
                action        = "重置番茄钟";
                request_error = app_pomodoro_request_reset();
                break;
            case APP_POMODORO_RUN_STATE_DONE:
                action        = "确认完成阶段";
                request_error = app_pomodoro_request_confirm();
                break;
            case APP_POMODORO_RUN_STATE_IDLE:
            default:
                return false;
        }
        if (request_error != ESP_OK)
        {
            ESP_LOGW(TAG, "%s请求未被接受: %s", action, esp_err_to_name(request_error));
        }
        return true;
    }
    return false;
}
