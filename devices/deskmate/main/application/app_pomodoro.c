/**
 * @file app_pomodoro.c
 * @brief 管理番茄钟生命周期资源并提供线程安全公共命令入口
 */
#include "app_pomodoro_internal.h"

#include <limits.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "system_clock.h"

#define APP_POMODORO_QUEUE_LENGTH     12U
#define APP_POMODORO_TASK_STACK_SIZE  5120U
#define APP_POMODORO_TASK_PRIORITY    3U
#define APP_POMODORO_START_TIMEOUT_MS 2000U

static const char *TAG                = "app_pomodoro";

app_pomodoro_context_t g_app_pomodoro = {
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
    taskENTER_CRITICAL(&g_app_pomodoro.timer_lock);
    const uint64_t generation = g_app_pomodoro.scheduled_generation;
    taskEXIT_CRITICAL(&g_app_pomodoro.timer_lock);
    const app_pomodoro_command_t command = {
        .type       = APP_POMODORO_COMMAND_TICK,
        .generation = generation,
    };
    if (g_app_pomodoro.queue != NULL && xQueueSend(g_app_pomodoro.queue, &command, 0) != pdTRUE)
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
    if (g_app_pomodoro.queue != NULL && xQueueSend(g_app_pomodoro.queue, &command, 0) != pdTRUE)
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
    if (g_app_pomodoro.running && !g_app_pomodoro.stopping && g_app_pomodoro.queue != NULL)
    {
        (void) xQueueSend(g_app_pomodoro.queue, &command, 0);
    }
}

/** @brief 删除当前已创建的生命周期资源 */
static void delete_resources(void)
{
    if (g_app_pomodoro.phase_timer != NULL)
    {
        (void) esp_timer_delete(g_app_pomodoro.phase_timer);
    }
    if (g_app_pomodoro.date_timer != NULL)
    {
        (void) esp_timer_delete(g_app_pomodoro.date_timer);
    }
    if (g_app_pomodoro.reconcile_sem != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro.reconcile_sem);
    }
    if (g_app_pomodoro.reconcile_lock != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro.reconcile_lock);
    }
    if (g_app_pomodoro.stopped_sem != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro.stopped_sem);
    }
    if (g_app_pomodoro.ready_sem != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro.ready_sem);
    }
    if (g_app_pomodoro.state_lock != NULL)
    {
        vSemaphoreDelete(g_app_pomodoro.state_lock);
    }
    if (g_app_pomodoro.queue != NULL)
    {
        vQueueDelete(g_app_pomodoro.queue);
    }
    const app_pomodoro_context_t empty = {
        .timer_lock = portMUX_INITIALIZER_UNLOCKED,
    };
    g_app_pomodoro = empty;
}

esp_err_t app_pomodoro_init(void)
{
    if (g_app_pomodoro.initialized)
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

    g_app_pomodoro.queue          = xQueueCreate(APP_POMODORO_QUEUE_LENGTH, sizeof(app_pomodoro_command_t));
    g_app_pomodoro.state_lock     = xSemaphoreCreateMutex();
    g_app_pomodoro.ready_sem      = xSemaphoreCreateBinary();
    g_app_pomodoro.stopped_sem    = xSemaphoreCreateBinary();
    g_app_pomodoro.reconcile_lock = xSemaphoreCreateMutex();
    g_app_pomodoro.reconcile_sem  = xSemaphoreCreateBinary();
    if (g_app_pomodoro.queue == NULL || g_app_pomodoro.state_lock == NULL || g_app_pomodoro.ready_sem == NULL
        || g_app_pomodoro.stopped_sem == NULL || g_app_pomodoro.reconcile_lock == NULL
        || g_app_pomodoro.reconcile_sem == NULL)
    {
        delete_resources();
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t phase_timer_args = {
        .callback = phase_timer_callback,
        .name     = "pomodoro_phase",
    };
    error = esp_timer_create(&phase_timer_args, &g_app_pomodoro.phase_timer);
    if (error != ESP_OK)
    {
        delete_resources();
        return error;
    }
    const esp_timer_create_args_t date_timer_args = {
        .callback = date_timer_callback,
        .name     = "pomodoro_date",
    };
    error = esp_timer_create(&date_timer_args, &g_app_pomodoro.date_timer);
    if (error != ESP_OK)
    {
        delete_resources();
        return error;
    }

    app_pomodoro_state_t initial = { 0 };
    initial.status.settings = stored.settings_valid
                                  ? (app_pomodoro_settings_t) {
                                        .focus_minutes       = stored.settings.focus_minutes,
                                        .short_break_minutes = stored.settings.short_break_minutes,
                                        .long_break_minutes  = stored.settings.long_break_minutes,
                                        .long_break_interval = stored.settings.long_break_interval,
                                    }
                                  : DEFAULT_SETTINGS;
    initial.status.phase                  = APP_POMODORO_PHASE_NONE;
    initial.status.next_phase             = APP_POMODORO_PHASE_FOCUS;
    initial.status.run_state              = APP_POMODORO_RUN_STATE_IDLE;
    initial.status.remaining_seconds      = (uint32_t) initial.status.settings.focus_minutes * 60U;
    initial.status.phase_duration_seconds = initial.status.remaining_seconds;
    initial.status.settings_saved         = stored.schema_valid && stored.settings_valid && stored.counts_valid;
    initial.status.generation             = 1U;
    if (stored.schema_valid && stored.counts_valid)
    {
        initial.today_date                 = stored.today_date;
        initial.status.today_focus_count   = stored.today_count;
        initial.status.pending_focus_count = stored.pending_count;
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
        error                         = pomodoro_store_save_settings_copy(&defaults);
        initial.status.settings_saved = error == ESP_OK;
        initial.status.last_error     = error;
        if (error != ESP_OK)
        {
            ESP_LOGW(TAG, "恢复默认番茄钟设置失败: %s", esp_err_to_name(error));
        }
    }
    if (stored.schema_valid && !stored.counts_valid)
    {
        ESP_LOGW(TAG, "番茄钟完成计数字段无效，已从零恢复");
    }

    g_app_pomodoro.state       = initial;
    g_app_pomodoro.initialized = true;
    error                      = system_clock_register_listener_borrow(on_system_clock_updated, NULL);
    if (error != ESP_OK)
    {
        delete_resources();
        return error;
    }
    ESP_LOGI(TAG,
             "番茄钟已初始化，专注=%u 分钟，长休间隔=%u 轮",
             (unsigned) initial.status.settings.focus_minutes,
             (unsigned) initial.status.settings.long_break_interval);
    return ESP_OK;
}

esp_err_t app_pomodoro_start(void)
{
    if (!g_app_pomodoro.initialized || g_app_pomodoro.running || g_app_pomodoro.task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(g_app_pomodoro.ready_sem, 0) == pdTRUE)
    {
    }
    while (xSemaphoreTake(g_app_pomodoro.stopped_sem, 0) == pdTRUE)
    {
    }
    g_app_pomodoro.running  = true;
    g_app_pomodoro.stopping = false;
    if (xTaskCreate(app_pomodoro_task,
                    "app_pomodoro_task",
                    APP_POMODORO_TASK_STACK_SIZE,
                    NULL,
                    APP_POMODORO_TASK_PRIORITY,
                    &g_app_pomodoro.task)
        != pdPASS)
    {
        g_app_pomodoro.running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(g_app_pomodoro.ready_sem, pdMS_TO_TICKS(APP_POMODORO_START_TIMEOUT_MS)) != pdTRUE)
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
    if (!g_app_pomodoro.initialized || !g_app_pomodoro.running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    if (!g_app_pomodoro.stopping)
    {
        const app_pomodoro_command_t command = { .type = APP_POMODORO_COMMAND_STOP };
        if (xQueueSend(g_app_pomodoro.queue, &command, timeout) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }
        g_app_pomodoro.stopping = true;
    }
    const TickType_t elapsed   = xTaskGetTickCount() - started;
    const TickType_t remaining = elapsed < timeout ? timeout - elapsed : 0U;
    if (xSemaphoreTake(g_app_pomodoro.stopped_sem, remaining) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    g_app_pomodoro.running  = false;
    g_app_pomodoro.stopping = false;
    (void) xQueueReset(g_app_pomodoro.queue);
    return ESP_OK;
}

esp_err_t app_pomodoro_deinit(void)
{
    if (!g_app_pomodoro.initialized || g_app_pomodoro.running || g_app_pomodoro.task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = system_clock_unregister_listener(on_system_clock_updated, NULL);
    if (error != ESP_OK)
    {
        return error;
    }
    delete_resources();
    return ESP_OK;
}

static esp_err_t enqueue_command(const app_pomodoro_command_t *command)
{
    if (command == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro.running || g_app_pomodoro.stopping || g_app_pomodoro.queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(g_app_pomodoro.queue, command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
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

esp_err_t app_pomodoro_request_update_settings_copy(const app_pomodoro_settings_t *settings)
{
    if (!app_pomodoro_settings_are_valid(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const app_pomodoro_command_t command = {
        .type     = APP_POMODORO_COMMAND_UPDATE_SETTINGS,
        .settings = *settings,
    };
    return enqueue_command(&command);
}

esp_err_t app_pomodoro_get_status_copy(app_pomodoro_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro.initialized || g_app_pomodoro.state_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_app_pomodoro.state_lock, portMAX_DELAY);
    *out_status = g_app_pomodoro.state.status;
    if (out_status->run_state == APP_POMODORO_RUN_STATE_RUNNING)
    {
        const int64_t remaining_us    = g_app_pomodoro.state.phase_deadline_us - esp_timer_get_time();
        out_status->remaining_seconds = remaining_us > 0 ? (uint32_t) ((remaining_us + 999999LL) / 1000000LL) : 0U;
    }
    xSemaphoreGive(g_app_pomodoro.state_lock);
    return ESP_OK;
}

esp_err_t app_pomodoro_get_next_wakeup_interval_ms(uint32_t *out_interval_ms)
{
    if (out_interval_ms == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_app_pomodoro.initialized || g_app_pomodoro.state_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_app_pomodoro.state_lock, portMAX_DELAY);
    if (g_app_pomodoro.state.status.run_state != APP_POMODORO_RUN_STATE_RUNNING)
    {
        xSemaphoreGive(g_app_pomodoro.state_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const int64_t remaining_us = g_app_pomodoro.state.phase_deadline_us - esp_timer_get_time();
    xSemaphoreGive(g_app_pomodoro.state_lock);
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
    if (!g_app_pomodoro.running || g_app_pomodoro.stopping)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(g_app_pomodoro.reconcile_lock, timeout) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    while (xSemaphoreTake(g_app_pomodoro.reconcile_sem, 0) == pdTRUE)
    {
    }
    uint32_t request_id = ++g_app_pomodoro.next_request_id;
    if (request_id == 0U)
    {
        request_id = ++g_app_pomodoro.next_request_id;
    }
    const app_pomodoro_command_t command = {
        .type       = APP_POMODORO_COMMAND_RECONCILE,
        .request_id = request_id,
    };
    const TickType_t started = xTaskGetTickCount();
    if (xQueueSend(g_app_pomodoro.queue, &command, timeout) != pdTRUE)
    {
        xSemaphoreGive(g_app_pomodoro.reconcile_lock);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    for (;;)
    {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout || xSemaphoreTake(g_app_pomodoro.reconcile_sem, timeout - elapsed) != pdTRUE)
        {
            break;
        }
        if (g_app_pomodoro.completed_request_id == request_id)
        {
            *out_result = g_app_pomodoro.reconcile_result;
            result      = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(g_app_pomodoro.reconcile_lock);
    return result;
}

bool app_pomodoro_consume_input(device_button_event_t key_event)
{
    if (key_event == DEVICE_BUTTON_EVENT_LEFT_SHORT || key_event == DEVICE_BUTTON_EVENT_RIGHT_SHORT)
    {
        return false;
    }
    app_pomodoro_status_t status;
    if (app_pomodoro_get_status_copy(&status) != ESP_OK)
    {
        return false;
    }
    if (key_event == DEVICE_BUTTON_EVENT_LEFT_LONG)
    {
        switch (status.run_state)
        {
            case APP_POMODORO_RUN_STATE_IDLE:
                (void) app_pomodoro_request_start();
                return true;
            case APP_POMODORO_RUN_STATE_RUNNING:
            case APP_POMODORO_RUN_STATE_PAUSED:
                (void) app_pomodoro_request_toggle_pause();
                return true;
            case APP_POMODORO_RUN_STATE_DONE:
                (void) app_pomodoro_request_confirm();
                return true;
            default:
                return false;
        }
    }
    if (key_event == DEVICE_BUTTON_EVENT_RIGHT_LONG)
    {
        switch (status.run_state)
        {
            case APP_POMODORO_RUN_STATE_RUNNING:
                (void) app_pomodoro_request_skip();
                break;
            case APP_POMODORO_RUN_STATE_PAUSED:
            case APP_POMODORO_RUN_STATE_DONE:
                (void) app_pomodoro_request_reset();
                break;
            case APP_POMODORO_RUN_STATE_IDLE:
            default:
                break;
        }
        return true;
    }
    return false;
}
