/*
 * 文件职责：维护设置菜单与网页控制台安全退出门控，并把物理按键转换为设置动作。
 */
#include "app_settings.h"

#include "app_network.h"
#include "app_ota.h"
#include "app_web_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "presentation_dispatch.h"

static const char  *TAG         = "app_settings";
static portMUX_TYPE s_menu_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         s_menu_active;

esp_err_t app_settings_request_portal(void)
{
    return app_network_request_start_portal();
}

esp_err_t app_settings_reset(void)
{
    const esp_err_t stop_error = app_web_console_request_stop();
    if (stop_error != ESP_OK)
    {
        return stop_error;
    }

    app_web_console_status_t web_console_status;
    const esp_err_t          status_error = app_web_console_get_status_copy(&web_console_status);
    if (status_error != ESP_OK)
    {
        return status_error;
    }
    if (web_console_status.state != APP_WEB_CONSOLE_STATE_STOPPED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = app_ota_clear_pending_update();
    if (error != ESP_OK)
    {
        return error;
    }
    taskENTER_CRITICAL(&s_menu_lock);
    s_menu_active = false;
    taskEXIT_CRITICAL(&s_menu_lock);
    return ESP_OK;
}

bool app_settings_consume_input(device_button_event_t key_event)
{
    taskENTER_CRITICAL(&s_menu_lock);
    const bool menu_active = s_menu_active;
    taskEXIT_CRITICAL(&s_menu_lock);

    if (!menu_active)
    {
        if (key_event != DEVICE_BUTTON_EVENT_RIGHT_LONG)
        {
            return false;
        }

        taskENTER_CRITICAL(&s_menu_lock);
        s_menu_active = true;
        taskEXIT_CRITICAL(&s_menu_lock);
        const esp_err_t error = presentation_dispatch_settings_action(PRESENTATION_SETTINGS_ACTION_OPEN);
        if (error != ESP_OK)
        {
            taskENTER_CRITICAL(&s_menu_lock);
            s_menu_active = false;
            taskEXIT_CRITICAL(&s_menu_lock);
            ESP_LOGW(TAG, "打开设置菜单动作投递失败: %s", esp_err_to_name(error));
        }
        return true;
    }

    presentation_settings_action_t action;
    switch (key_event)
    {
        case DEVICE_BUTTON_EVENT_LEFT_SHORT:
            action = PRESENTATION_SETTINGS_ACTION_PREV;
            break;
        case DEVICE_BUTTON_EVENT_RIGHT_SHORT:
            action = PRESENTATION_SETTINGS_ACTION_NEXT;
            break;
        case DEVICE_BUTTON_EVENT_LEFT_LONG:
            action = PRESENTATION_SETTINGS_ACTION_BACK;
            break;
        case DEVICE_BUTTON_EVENT_RIGHT_LONG:
            action = PRESENTATION_SETTINGS_ACTION_ACTIVATE;
            break;
        default:
            return true;
    }

    const esp_err_t error = presentation_dispatch_settings_action(action);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "设置菜单动作投递失败: action=%u err=%s", (unsigned) action, esp_err_to_name(error));
    }
    return true;
}
