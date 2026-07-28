/**
 * @file rtc_service_task.c
 * @brief 在独立 Task 中消费 RTC GPIO 中断并清除 PCF85063 告警标志
 */
#include "rtc_service.h"

#include "device_rtc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "task_stack_stats.h"

#define RTC_SERVICE_TASK_STACK_SIZE_BYTES 3072U
#define RTC_SERVICE_TASK_PRIORITY         4U
#define RTC_SERVICE_NOTIFY_INTERRUPT      (1UL << 0)
#define RTC_SERVICE_NOTIFY_STOP           (1UL << 1)
#define RTC_SERVICE_INTERNAL_STOP_MS      1000U
#define RTC_SERVICE_RETRY_BASE_DELAY_MS   1000U
#define RTC_SERVICE_RETRY_MAX_ATTEMPTS    3U

static const char *TAG = "rtc_service";

static rtc_service_state_t          s_state;
static uint32_t                     s_alarm_count;
static esp_err_t                    s_last_error;
static rtc_service_event_callback_t s_event_callback;
static void                        *s_event_context;
static TaskHandle_t                 s_task;
static SemaphoreHandle_t            s_stopped_signal;
static StaticSemaphore_t            s_stopped_signal_buffer;
static portMUX_TYPE                 s_state_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 在内部锁外发布不可变事件
 *
 * @param[in] id 事件类型
 * @param[in] result 本次硬件事务结果
 */
static void publish_event(rtc_service_event_id_t id, esp_err_t result)
{
    rtc_service_event_callback_t callback;
    void                        *context;
    uint32_t                     alarm_count;
    taskENTER_CRITICAL(&s_state_lock);
    callback    = s_event_callback;
    context     = s_event_context;
    alarm_count = s_alarm_count;
    taskEXIT_CRITICAL(&s_state_lock);

    if (callback != NULL)
    {
        const rtc_service_event_t event = {
            .id          = id,
            .result      = result,
            .alarm_count = alarm_count,
        };
        callback(&event, context);
    }
}

/** @brief 将第 N 次失败重试转换为有界的指数退避 Tick 数 */
static TickType_t retry_delay_ticks(uint32_t retry_attempt)
{
    const uint32_t   delay_ms = RTC_SERVICE_RETRY_BASE_DELAY_MS << (retry_attempt - 1U);
    const TickType_t ticks    = pdMS_TO_TICKS(delay_ms);
    return ticks == 0U ? 1U : ticks;
}

/**
 * @brief 在普通 Task 上下文核实并清除 AF
 *
 * GPIO ISR 只提供“INT 出现下降沿”的事实；本函数通过 I2C 读取 AF，确认属于告警后先清除
 * 标志释放低电平 INT，再向 Application 报告事件。
 *
 * @return ESP_OK 未发现 AF 或已成功消费；其他值表示 I2C 事务失败
 */
static esp_err_t consume_alarm_interrupt(void)
{
    bool      pending = false;
    esp_err_t error   = device_rtc_read_alarm_flag(&pending);
    if (error == ESP_OK && !pending)
    {
        return ESP_OK;
    }
    if (error == ESP_OK)
    {
        error = device_rtc_clear_alarm_flag();
    }

    uint32_t alarm_count;
    taskENTER_CRITICAL(&s_state_lock);
    s_last_error = error;
    if (error == ESP_OK)
    {
        ++s_alarm_count;
    }
    alarm_count = s_alarm_count;
    taskEXIT_CRITICAL(&s_state_lock);

    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "RTC 告警已消费，累计=%lu", (unsigned long) alarm_count);
        publish_event(RTC_SERVICE_EVENT_ALARM_TRIGGERED, ESP_OK);
    }
    else
    {
        ESP_LOGE(TAG, "RTC 告警标志读取或清除失败: %s", esp_err_to_name(error));
        publish_event(RTC_SERVICE_EVENT_PROCESSING_FAILED, error);
    }
    return error;
}

/** @brief RTC GPIO ISR 回调，只向唯一 Service Task 投递通知位 */
static void rtc_service_interrupt_callback(void *context)
{
    (void) context;
    portENTER_CRITICAL_ISR(&s_state_lock);
    TaskHandle_t task = s_state == RTC_SERVICE_STATE_RUNNING ? s_task : NULL;
    portEXIT_CRITICAL_ISR(&s_state_lock);
    if (task == NULL)
    {
        return;
    }

    BaseType_t high_task_woken = pdFALSE;
    (void) xTaskNotifyFromISR(task, RTC_SERVICE_NOTIFY_INTERRUPT, eSetBits, &high_task_woken);
    if (high_task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

/** @brief RTC INT 唯一消费 Task，串行执行 AF I2C 事务和上层回调 */
static void rtc_service_task(void *context)
{
    (void) context;
    task_stack_stats_t stack_stats    = TASK_STACK_STATS_INITIALIZER;
    bool               retry_pending  = false;
    uint32_t           retry_attempts = 0U;
    for (;;)
    {
        task_stack_stats_log_if_due(&stack_stats, "rtc_service");
        uint32_t         notification = 0;
        const BaseType_t notified = xTaskNotifyWait(0U,
                                                    UINT32_MAX,
                                                    &notification,
                                                    retry_pending ? retry_delay_ticks(retry_attempts) : portMAX_DELAY);
        if ((notification & RTC_SERVICE_NOTIFY_STOP) != 0U)
        {
            break;
        }
        const bool retry_due   = notified != pdTRUE && retry_pending;
        const bool interrupted = (notification & RTC_SERVICE_NOTIFY_INTERRUPT) != 0U;
        if (!retry_due && !interrupted)
        {
            continue;
        }

        const esp_err_t error = consume_alarm_interrupt();
        if (error == ESP_OK)
        {
            retry_pending  = false;
            retry_attempts = 0U;
        }
        else if (error == ESP_ERR_INVALID_STATE)
        {
            retry_pending  = false;
            retry_attempts = 0U;
        }
        else if (retry_attempts < RTC_SERVICE_RETRY_MAX_ATTEMPTS)
        {
            ++retry_attempts;
            retry_pending = true;
            ESP_LOGW(TAG,
                     "RTC 告警消费失败，将在 %lu ms 后第 %lu 次重试",
                     (unsigned long) (RTC_SERVICE_RETRY_BASE_DELAY_MS << (retry_attempts - 1U)),
                     (unsigned long) retry_attempts);
        }
        else
        {
            retry_pending  = false;
            retry_attempts = 0U;
            ESP_LOGE(TAG, "RTC 告警消费重试已耗尽，等待后续显式检查");
        }
    }

    task_stack_stats_log_now("rtc_service");
    taskENTER_CRITICAL(&s_state_lock);
    s_task  = NULL;
    s_state = RTC_SERVICE_STATE_INITIALIZED;
    taskEXIT_CRITICAL(&s_state_lock);
    (void) xSemaphoreGive(s_stopped_signal);
    vTaskDelete(NULL);
}

esp_err_t rtc_service_init(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool uninitialized = s_state == RTC_SERVICE_STATE_UNINITIALIZED;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!uninitialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_stopped_signal = xSemaphoreCreateBinaryStatic(&s_stopped_signal_buffer);
    if (s_stopped_signal == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    taskENTER_CRITICAL(&s_state_lock);
    s_alarm_count = 0U;
    s_last_error  = ESP_OK;
    s_state       = RTC_SERVICE_STATE_INITIALIZED;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t rtc_service_set_event_callback_borrow(rtc_service_event_callback_t callback, void *context)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != RTC_SERVICE_STATE_INITIALIZED)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_event_callback = callback;
    s_event_context  = callback != NULL ? context : NULL;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t rtc_service_start(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != RTC_SERVICE_STATE_INITIALIZED)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = RTC_SERVICE_STATE_STARTING;
    taskEXIT_CRITICAL(&s_state_lock);

    while (xSemaphoreTake(s_stopped_signal, 0U) == pdTRUE)
    {
    }
    BaseType_t created = xTaskCreate(rtc_service_task,
                                     "rtc_service",
                                     RTC_SERVICE_TASK_STACK_SIZE_BYTES,
                                     NULL,
                                     RTC_SERVICE_TASK_PRIORITY,
                                     &s_task);
    if (created != pdPASS)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_task  = NULL;
        s_state = RTC_SERVICE_STATE_INITIALIZED;
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = device_rtc_set_interrupt_callback_borrow(rtc_service_interrupt_callback, NULL);
    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_state = RTC_SERVICE_STATE_STOPPING;
        taskEXIT_CRITICAL(&s_state_lock);
        (void) xTaskNotify(s_task, RTC_SERVICE_NOTIFY_STOP, eSetBits);
        (void) xSemaphoreTake(s_stopped_signal, pdMS_TO_TICKS(RTC_SERVICE_INTERNAL_STOP_MS));
        return error;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_state = RTC_SERVICE_STATE_RUNNING;
    taskEXIT_CRITICAL(&s_state_lock);
    (void) xTaskNotify(s_task, RTC_SERVICE_NOTIFY_INTERRUPT, eSetBits);
    ESP_LOGI(TAG, "RTC INT 消费 Service 已启动");
    return ESP_OK;
}

esp_err_t rtc_service_request_check(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != RTC_SERVICE_STATE_RUNNING || s_task == NULL)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    TaskHandle_t task = s_task;
    taskEXIT_CRITICAL(&s_state_lock);
    return xTaskNotify(task, RTC_SERVICE_NOTIFY_INTERRUPT, eSetBits) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t rtc_service_stop(uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (s_state == RTC_SERVICE_STATE_INITIALIZED && s_task == NULL)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    if (s_state != RTC_SERVICE_STATE_RUNNING && s_state != RTC_SERVICE_STATE_STOPPING)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool   begin_stop = s_state == RTC_SERVICE_STATE_RUNNING;
    TaskHandle_t task       = s_task;
    if (begin_stop)
    {
        s_state = RTC_SERVICE_STATE_STOPPING;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (begin_stop)
    {
        const esp_err_t unregister_error = device_rtc_set_interrupt_callback_borrow(NULL, NULL);
        if (unregister_error != ESP_OK)
        {
            taskENTER_CRITICAL(&s_state_lock);
            if (s_state == RTC_SERVICE_STATE_STOPPING && s_task == task)
            {
                s_state = RTC_SERVICE_STATE_RUNNING;
            }
            taskEXIT_CRITICAL(&s_state_lock);
            return unregister_error;
        }

        (void) xTaskNotify(task, RTC_SERVICE_NOTIFY_STOP, eSetBits);
    }

    if (xSemaphoreTake(s_stopped_signal, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        taskENTER_CRITICAL(&s_state_lock);
        const bool stopped = s_state == RTC_SERVICE_STATE_INITIALIZED && s_task == NULL;
        taskEXIT_CRITICAL(&s_state_lock);
        if (stopped)
        {
            ESP_LOGI(TAG, "RTC INT 消费 Service 已停止");
            return ESP_OK;
        }
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "RTC INT 消费 Service 已停止");
    return ESP_OK;
}

esp_err_t rtc_service_get_status_copy(rtc_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_state_lock);
    *out_status = (rtc_service_status_t) {
        .state       = s_state,
        .alarm_count = s_alarm_count,
        .last_error  = s_last_error,
    };
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t rtc_service_deinit(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (s_state != RTC_SERVICE_STATE_INITIALIZED)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state          = RTC_SERVICE_STATE_UNINITIALIZED;
    s_alarm_count    = 0U;
    s_last_error     = ESP_OK;
    s_event_callback = NULL;
    s_event_context  = NULL;
    taskEXIT_CRITICAL(&s_state_lock);

    vSemaphoreDelete(s_stopped_signal);
    s_stopped_signal = NULL;
    return ESP_OK;
}
