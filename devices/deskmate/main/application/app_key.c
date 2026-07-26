/*
 * 文件职责：把按键事件解释成 App 业务命令。
 */
#include "app_key.h"

#include "app_page.h"
#include "app_power.h"
#include "button_service.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "app_key";

ESP_EVENT_DEFINE_BASE(APP_KEY_EVENT);

typedef struct
{
    device_button_event_t event;
    uint32_t              timestamp_ms;
} app_key_event_data_t;

/**
 * @brief 在 ESP Timer Task 上下文只复制并投递一个小型输入事件
 */
static void on_button_service_event(device_button_event_t event, uint32_t timestamp_ms, void *context)
{
    (void) context;

    /*
     * 先更新活动代次，再向默认事件循环投递业务事件。这样即使事件循环正忙或队列已满，
     * 电源 Application 也不会在一个已经确认的按键事实之后继续进入轻睡眠。
     */
    (void) app_power_notify_activity();
    const app_key_event_data_t data = {
        .event        = event,
        .timestamp_ms = timestamp_ms,
    };
    const esp_err_t error = esp_event_post(APP_KEY_EVENT, event, &data, sizeof(data), 0);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "投递 App 按键事件失败: event=%d err=%s", (int) event, esp_err_to_name(error));
    }
}

/**
 * @brief 在默认事件循环上下文执行可能涉及其他产品 Queue 的输入策略
 */
static void on_app_key_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void) arg;
    (void) base;

    device_button_event_t event = (device_button_event_t) id;
    if (event_data != NULL)
    {
        event = ((const app_key_event_data_t *) event_data)->event;
    }
    (void) app_key_dispatch_event(event);
}

esp_err_t app_key_init(void)
{
    esp_err_t error = esp_event_handler_register(APP_KEY_EVENT, ESP_EVENT_ANY_ID, on_app_key_event, NULL);
    if (error != ESP_OK)
    {
        return error;
    }

    error = button_service_set_event_callback_borrow(on_button_service_event, NULL);
    if (error != ESP_OK)
    {
        (void) esp_event_handler_unregister(APP_KEY_EVENT, ESP_EVENT_ANY_ID, on_app_key_event);
    }
    return error;
}

bool app_key_dispatch_event(device_button_event_t key_event)
{
    ESP_LOGI(TAG, "输入业务事件: event=%d", (int) key_event);
    if (app_page_is_transitioning())
    {
        return true;
    }
    if (app_page_consume_input(key_event))
    {
        return true;
    }
    return false;
}
