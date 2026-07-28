/**
 * @file button_service.c
 * @brief 实现按键边沿触发的 one-shot 状态机调度与事件转发
 */
#include "button_service.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BUTTON_SERVICE_START_ROLLBACK_TIMEOUT_MS 1000U
#define BUTTON_WAKE_LEFT                         (1U << 0)
#define BUTTON_WAKE_RIGHT                        (1U << 1)

typedef enum
{
    BUTTON_SERVICE_STATE_UNINITIALIZED = 0,
    BUTTON_SERVICE_STATE_INITIALIZED,
    BUTTON_SERVICE_STATE_RUNNING,
    BUTTON_SERVICE_STATE_STOPPING,
} button_service_state_t;

typedef struct
{
    bool      edge_pending;
    bool      timer_armed;
    uint32_t  activity_inflight;
    uint32_t  schedule_inflight;
    uint32_t  timer_inflight;
    esp_err_t schedule_error;
    uint8_t   wake_pending_mask;
} button_service_runtime_t;

static const char *TAG                  = "button_service";

static portMUX_TYPE              s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t         s_quiesced_signal;
static button_service_state_t    s_state;
static button_service_runtime_t  s_runtime;
static esp_timer_handle_t        s_scan_timer;
static button_service_event_cb_t s_event_callback;
static void                     *s_event_context;
static uint32_t                  s_scan_period_ms;
static bool                      s_scan_error_reported;

static esp_err_t schedule_scan_from_task(void);

/** @brief 把产品按键事件映射为对应的 Light Sleep 唤醒事实位 */
static uint8_t event_wake_mask(device_button_event_t event)
{
    switch (event)
    {
        case DEVICE_BUTTON_EVENT_LEFT_SHORT:
        case DEVICE_BUTTON_EVENT_LEFT_LONG:
            return BUTTON_WAKE_LEFT;
        case DEVICE_BUTTON_EVENT_RIGHT_SHORT:
        case DEVICE_BUTTON_EVENT_RIGHT_LONG:
            return BUTTON_WAKE_RIGHT;
        default:
            return 0U;
    }
}

/** @brief 在停止等待期间从普通上下文通知一次状态变化 */
static void signal_stopping_from_task(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool stopping = s_state == BUTTON_SERVICE_STATE_STOPPING;
    taskEXIT_CRITICAL(&s_lock);
    if (stopping)
    {
        (void) xSemaphoreGive(s_quiesced_signal);
    }
}

/**
 * @brief 从 GPIO ISR 锁存活动事实，并在没有待执行 Timer 时安排一次扫描
 *
 * 只调用 ISR-safe API；任何调度错误均锁存到普通 Timer Task 上下文处理。
 */
static void IRAM_ATTR button_service_activity_isr(void *context)
{
    (void) context;
    bool      should_schedule = false;
    esp_err_t schedule_error  = ESP_OK;

    taskENTER_CRITICAL_ISR(&s_lock);
    s_runtime.activity_inflight++;
    if (s_state == BUTTON_SERVICE_STATE_RUNNING)
    {
        s_runtime.edge_pending = true;
        if (!s_runtime.timer_armed)
        {
            s_runtime.timer_armed = true;
            s_runtime.schedule_inflight++;
            should_schedule = true;
        }
    }
    taskEXIT_CRITICAL_ISR(&s_lock);

    if (should_schedule)
    {
        schedule_error = esp_timer_start_once(s_scan_timer, (uint64_t) s_scan_period_ms * 1000ULL);
        taskENTER_CRITICAL_ISR(&s_lock);
        s_runtime.schedule_inflight--;
        if (schedule_error != ESP_OK)
        {
            s_runtime.timer_armed = false;
            if (s_runtime.schedule_error == ESP_OK)
            {
                s_runtime.schedule_error = schedule_error;
            }
        }
        taskEXIT_CRITICAL_ISR(&s_lock);
    }

    BaseType_t higher_priority_task_woken = pdFALSE;
    taskENTER_CRITICAL_ISR(&s_lock);
    s_runtime.activity_inflight--;
    const bool stopping = s_state == BUTTON_SERVICE_STATE_STOPPING;
    taskEXIT_CRITICAL_ISR(&s_lock);
    if (stopping)
    {
        (void) xSemaphoreGiveFromISR(s_quiesced_signal, &higher_priority_task_woken);
    }
    if (higher_priority_task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief 从普通上下文幂等安排一次扫描
 * @return ESP_OK 已有或已成功安排 Timer；ESP_ERR_INVALID_STATE 未运行；或 ESP Timer 错误码
 */
static esp_err_t schedule_scan_from_task(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_state != BUTTON_SERVICE_STATE_RUNNING)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.timer_armed)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_runtime.timer_armed = true;
    s_runtime.schedule_inflight++;
    taskEXIT_CRITICAL(&s_lock);

    const esp_err_t error = esp_timer_start_once(s_scan_timer, (uint64_t) s_scan_period_ms * 1000ULL);

    taskENTER_CRITICAL(&s_lock);
    s_runtime.schedule_inflight--;
    if (error != ESP_OK)
    {
        s_runtime.timer_armed = false;
        if (s_runtime.schedule_error == ESP_OK)
        {
            s_runtime.schedule_error = error;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    signal_stopping_from_task();
    return error;
}

/** @brief 首次记录连续扫描故障，避免相同故障每 10 ms 重复输出 */
static void report_scan_failure(const char *operation, esp_err_t error)
{
    if (!s_scan_error_reported)
    {
        ESP_LOGE(TAG, "%s，将在下一轮按需扫描时重试: %s", operation, esp_err_to_name(error));
        s_scan_error_reported = true;
    }
}

/** @brief 在连续扫描故障后的首次成功扫描中记录恢复 */
static void report_scan_recovered(void)
{
    if (s_scan_error_reported)
    {
        ESP_LOGI(TAG, "按键按需扫描已恢复");
        s_scan_error_reported = false;
    }
}

/**
 * @brief 在 ESP Timer Task 上下文推进一次状态机并按需安排后续 one-shot
 *
 * `timer_inflight` 覆盖 Device 扫描和上层事件回调，供同步 stop() 等待。
 */
static void button_service_scan_timer_callback(void *context)
{
    (void) context;

    taskENTER_CRITICAL(&s_lock);
    s_runtime.timer_armed = false;
    s_runtime.timer_inflight++;
    if (s_state != BUTTON_SERVICE_STATE_RUNNING)
    {
        taskEXIT_CRITICAL(&s_lock);
        goto exit_callback;
    }
    s_runtime.edge_pending         = false;
    const esp_err_t schedule_error = s_runtime.schedule_error;
    s_runtime.schedule_error       = ESP_OK;
    taskEXIT_CRITICAL(&s_lock);

    if (schedule_error != ESP_OK)
    {
        report_scan_failure("按键扫描调度失败", schedule_error);
    }

    const uint32_t              now_ms      = (uint32_t) ((uint64_t) esp_timer_get_time() / 1000ULL);
    device_button_scan_result_t result      = { 0 };
    const esp_err_t             scan_error  = device_button_scan(now_ms, &result);
    bool                        scan_failed = scan_error != ESP_OK;
    device_button_event_t       events[DEVICE_BUTTON_MAX_EVENTS] = { DEVICE_BUTTON_EVENT_NONE };
    uint8_t                     event_count                      = 0U;
    if (scan_failed)
    {
        report_scan_failure("按键按需扫描失败", scan_error);
    }
    else
    {
        for (uint8_t index = 0; index < result.event_count; ++index)
        {
            events[event_count++] = result.events[index];
        }

        uint8_t device_event_mask = 0U;
        for (uint8_t index = 0; index < event_count; ++index)
        {
            device_event_mask |= event_wake_mask(events[index]);
        }

        taskENTER_CRITICAL(&s_lock);
        s_runtime.wake_pending_mask &= (uint8_t) ~device_event_mask;
        const uint8_t wake_to_resolve = s_runtime.wake_pending_mask;
        taskEXIT_CRITICAL(&s_lock);

        if (wake_to_resolve != 0U)
        {
            device_button_pressed_snapshot_t pressed_snapshot = { 0 };
            const esp_err_t                  pressed_error    = device_button_read_pressed_snapshot(&pressed_snapshot);
            if (pressed_error != ESP_OK)
            {
                report_scan_failure("读取唤醒按键物理状态失败", pressed_error);
                scan_failed = true;
            }
            else
            {
                uint8_t synthesized_mask = 0U;
                if ((wake_to_resolve & BUTTON_WAKE_LEFT) != 0U && !pressed_snapshot.left_pressed
                    && event_count < DEVICE_BUTTON_MAX_EVENTS)
                {
                    events[event_count++] = DEVICE_BUTTON_EVENT_LEFT_SHORT;
                    synthesized_mask |= BUTTON_WAKE_LEFT;
                }
                if ((wake_to_resolve & BUTTON_WAKE_RIGHT) != 0U && !pressed_snapshot.right_pressed
                    && event_count < DEVICE_BUTTON_MAX_EVENTS)
                {
                    events[event_count++] = DEVICE_BUTTON_EVENT_RIGHT_SHORT;
                    synthesized_mask |= BUTTON_WAKE_RIGHT;
                }

                taskENTER_CRITICAL(&s_lock);
                s_runtime.wake_pending_mask &= (uint8_t) ~synthesized_mask;
                taskEXIT_CRITICAL(&s_lock);
            }
        }

        if (!scan_failed)
        {
            report_scan_recovered();
        }
        for (uint8_t index = 0; index < event_count; ++index)
        {
            if (s_event_callback != NULL)
            {
                s_event_callback(events[index], now_ms, s_event_context);
            }
        }
    }

    taskENTER_CRITICAL(&s_lock);
    const bool running                  = s_state == BUTTON_SERVICE_STATE_RUNNING;
    const bool edge_arrived_during_scan = s_runtime.edge_pending;
    const bool timer_already_armed      = s_runtime.timer_armed;
    const bool wake_pending             = s_runtime.wake_pending_mask != 0U;
    taskEXIT_CRITICAL(&s_lock);

    if (running && !timer_already_armed
        && (result.follow_up_required || edge_arrived_during_scan || wake_pending || scan_failed))
    {
        const esp_err_t error = schedule_scan_from_task();
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
        {
            report_scan_failure("安排后续按键扫描失败", error);
        }
    }

exit_callback:
    taskENTER_CRITICAL(&s_lock);
    s_runtime.timer_inflight--;
    const bool stopping = s_state == BUTTON_SERVICE_STATE_STOPPING;
    taskEXIT_CRITICAL(&s_lock);
    if (stopping)
    {
        (void) xSemaphoreGive(s_quiesced_signal);
    }
}

/**
 * @brief 在同一个超时预算内等待指定在途计数归零
 * @param[in] wait_for_timer 是否同时等待 Timer 回调退出
 * @param[in] started_at_ticks stop() 开始时的 Tick
 * @param[in] timeout_ticks 整个 stop() 共用的超时预算
 * @return ESP_OK 已收敛；ESP_ERR_TIMEOUT 超时
 */
static esp_err_t wait_for_quiescence(bool wait_for_timer, TickType_t started_at_ticks, TickType_t timeout_ticks)
{
    for (;;)
    {
        taskENTER_CRITICAL(&s_lock);
        const bool quiesced = s_runtime.activity_inflight == 0U && s_runtime.schedule_inflight == 0U
                              && (!wait_for_timer || s_runtime.timer_inflight == 0U);
        taskEXIT_CRITICAL(&s_lock);
        if (quiesced)
        {
            return ESP_OK;
        }

        const TickType_t elapsed = xTaskGetTickCount() - started_at_ticks;
        if (elapsed >= timeout_ticks)
        {
            return ESP_ERR_TIMEOUT;
        }
        const TickType_t remaining = timeout_ticks - elapsed;
        if (xSemaphoreTake(s_quiesced_signal, remaining) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t button_service_init(const button_service_config_t *config)
{
    if (config == NULL || config->scan_period_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_lock);
    const bool uninitialized = s_state == BUTTON_SERVICE_STATE_UNINITIALIZED;
    taskEXIT_CRITICAL(&s_lock);
    if (!uninitialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_quiesced_signal = xSemaphoreCreateBinary();
    if (s_quiesced_signal == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_config = {
        .name                  = "button_scan",
        .callback              = button_service_scan_timer_callback,
        .arg                   = NULL,
        .dispatch_method       = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    const esp_err_t error = esp_timer_create(&timer_config, &s_scan_timer);
    if (error != ESP_OK)
    {
        vSemaphoreDelete(s_quiesced_signal);
        s_quiesced_signal = NULL;
        return error;
    }

    taskENTER_CRITICAL(&s_lock);
    s_scan_period_ms      = config->scan_period_ms;
    s_runtime             = (button_service_runtime_t) { 0 };
    s_state               = BUTTON_SERVICE_STATE_INITIALIZED;
    s_scan_error_reported = false;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "按键按需扫描 Service 初始化完成，活动推进间隔=%lu ms", (unsigned long) s_scan_period_ms);
    return ESP_OK;
}

esp_err_t button_service_set_event_callback_borrow(button_service_event_cb_t callback, void *context)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_state != BUTTON_SERVICE_STATE_INITIALIZED)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_event_callback = callback;
    s_event_context  = callback != NULL ? context : NULL;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t button_service_start(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_state != BUTTON_SERVICE_STATE_INITIALIZED)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime = (button_service_runtime_t) { 0 };
    taskEXIT_CRITICAL(&s_lock);

    esp_err_t error = device_button_set_activity_callback_borrow(button_service_activity_isr, NULL);
    if (error != ESP_OK)
    {
        return error;
    }

    taskENTER_CRITICAL(&s_lock);
    s_state = BUTTON_SERVICE_STATE_RUNNING;
    taskEXIT_CRITICAL(&s_lock);

    error = schedule_scan_from_task();
    if (error == ESP_OK)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_lock);
    s_state = BUTTON_SERVICE_STATE_STOPPING;
    taskEXIT_CRITICAL(&s_lock);
    const esp_err_t rollback_error = button_service_stop(BUTTON_SERVICE_START_ROLLBACK_TIMEOUT_MS);
    if (rollback_error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "启动按键按需扫描失败且回滚未收敛: start=%s rollback=%s",
                 esp_err_to_name(error),
                 esp_err_to_name(rollback_error));
        return rollback_error;
    }
    return error;
}

esp_err_t button_service_request_light_sleep_wakeup_copy(const button_service_wakeup_snapshot_t *wakeup_snapshot)
{
    if (wakeup_snapshot == NULL || (!wakeup_snapshot->left_button && !wakeup_snapshot->right_button))
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t wake_mask = 0U;
    if (wakeup_snapshot->left_button)
    {
        wake_mask |= BUTTON_WAKE_LEFT;
    }
    if (wakeup_snapshot->right_button)
    {
        wake_mask |= BUTTON_WAKE_RIGHT;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_state != BUTTON_SERVICE_STATE_RUNNING)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.wake_pending_mask |= wake_mask;
    taskEXIT_CRITICAL(&s_lock);
    return schedule_scan_from_task();
}

esp_err_t button_service_stop(uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_state != BUTTON_SERVICE_STATE_RUNNING && s_state != BUTTON_SERVICE_STATE_STOPPING)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = BUTTON_SERVICE_STATE_STOPPING;
    taskEXIT_CRITICAL(&s_lock);

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0U)
    {
        timeout_ticks = 1U;
    }
    const TickType_t started_at_ticks = xTaskGetTickCount();

    const esp_err_t callback_error    = device_button_set_activity_callback_borrow(NULL, NULL);
    if (callback_error != ESP_OK)
    {
        return callback_error;
    }

    esp_err_t error = wait_for_quiescence(false, started_at_ticks, timeout_ticks);
    if (error != ESP_OK)
    {
        return error;
    }

    error = esp_timer_stop(s_scan_timer);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        return error;
    }

    error = wait_for_quiescence(true, started_at_ticks, timeout_ticks);
    if (error != ESP_OK)
    {
        return error;
    }

    taskENTER_CRITICAL(&s_lock);
    s_runtime             = (button_service_runtime_t) { 0 };
    s_scan_error_reported = false;
    s_state               = BUTTON_SERVICE_STATE_INITIALIZED;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t button_service_deinit(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool initialized = s_state == BUTTON_SERVICE_STATE_INITIALIZED;
    taskEXIT_CRITICAL(&s_lock);
    if (!initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = esp_timer_delete(s_scan_timer);
    if (error != ESP_OK)
    {
        return error;
    }

    vSemaphoreDelete(s_quiesced_signal);
    taskENTER_CRITICAL(&s_lock);
    s_scan_timer          = NULL;
    s_quiesced_signal     = NULL;
    s_event_callback      = NULL;
    s_event_context       = NULL;
    s_scan_period_ms      = 0U;
    s_runtime             = (button_service_runtime_t) { 0 };
    s_scan_error_reported = false;
    s_state               = BUTTON_SERVICE_STATE_UNINITIALIZED;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
