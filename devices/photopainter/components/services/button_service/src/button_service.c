/**
 * @file button_service.c
 * @brief 实现按键状态机的持续周期调度与事件投递
 */
#include "button_service.h"

#include <stdbool.h>

#include "esp_log.h"
#include "system_periodic_timer.h"

/** @brief 按键状态机调度周期 */
#define BUTTON_SERVICE_SCAN_PERIOD_MS 10U

/** @brief 日志标签 */
static const char *TAG = "button_service";

/** @brief Service 拥有的周期执行资源 */
static system_periodic_timer_handle_t s_scan_timer;

/** @brief 上层长期借用的事件回调 */
static button_service_event_cb_t s_event_callback;

/** @brief 上层长期借用的事件回调上下文 */
static void                     *s_event_context;

/** @brief Service 生命周期与诊断状态 */
static bool s_initialized;
static bool s_running;
static bool s_scan_error_reported;

/** @brief 把 Device 不可变事件同步转发给 Service 消费者 */
static void button_service_on_device_event(device_button_id_t button, device_button_event_t event,
                                           uint8_t click_count, void *context)
{
    (void) context;
    if (s_event_callback != NULL)
    {
        s_event_callback(button, event, click_count, s_event_context);
    }
}

/**
 * @brief 周期推进 Device 状态机，并对连续失败进行一次性报告
 *
 * 下次周期会自然重试；恢复时输出一次恢复日志，避免持续故障每 10 ms 刷屏。
 */
static void button_service_scan_timer_callback(uint32_t elapsed_ms, void *context)
{
    (void) context;
    esp_err_t error = device_button_scan(elapsed_ms);
    if (error != ESP_OK)
    {
        if (!s_scan_error_reported)
        {
            ESP_LOGE(TAG, "按键周期扫描失败，将在下一周期重试: %s", esp_err_to_name(error));
            s_scan_error_reported = true;
        }
        return;
    }
    if (s_scan_error_reported)
    {
        ESP_LOGI(TAG, "按键周期扫描已恢复");
        s_scan_error_reported = false;
    }
}

esp_err_t button_service_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = device_button_set_event_callback_borrow(button_service_on_device_event, NULL);
    if (error != ESP_OK)
    {
        return error;
    }

    const system_periodic_timer_config_t timer_config = {
        .name                  = "button_scan",
        .callback              = button_service_scan_timer_callback,
        .context               = NULL,
        .skip_unhandled_events = true,
    };
    error = system_periodic_timer_create_borrow(&timer_config, &s_scan_timer);
    if (error != ESP_OK)
    {
        (void) device_button_set_event_callback_borrow(NULL, NULL);
        return error;
    }

    s_initialized         = true;
    s_scan_error_reported = false;
    ESP_LOGI(TAG, "按键持续扫描 Service 初始化完成");
    return ESP_OK;
}

esp_err_t button_service_set_event_callback_borrow(button_service_event_cb_t callback,
                                                   void                     *context)
{
    if (!s_initialized || s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_event_callback = callback;
    s_event_context  = callback != NULL ? context : NULL;
    return ESP_OK;
}

esp_err_t button_service_start(void)
{
    if (!s_initialized || s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = system_periodic_timer_start(s_scan_timer, BUTTON_SERVICE_SCAN_PERIOD_MS);
    if (error == ESP_OK)
    {
        s_running = true;
    }
    return error;
}

esp_err_t button_service_stop(void)
{
    if (!s_initialized || !s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = system_periodic_timer_stop(s_scan_timer);
    if (error == ESP_OK)
    {
        s_running             = false;
        s_scan_error_reported = false;
    }
    return error;
}

esp_err_t button_service_deinit(void)
{
    if (!s_initialized || s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = device_button_set_event_callback_borrow(NULL, NULL);
    if (error != ESP_OK)
    {
        return error;
    }
    error = system_periodic_timer_destroy(s_scan_timer);
    if (error != ESP_OK)
    {
        (void) device_button_set_event_callback_borrow(button_service_on_device_event, NULL);
        return error;
    }

    s_scan_timer          = NULL;
    s_event_callback      = NULL;
    s_event_context       = NULL;
    s_initialized         = false;
    s_scan_error_reported = false;
    return ESP_OK;
}
