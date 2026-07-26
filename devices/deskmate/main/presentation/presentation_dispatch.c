/*
 * 文件职责：把页面和 View Model 变化发布为不可变 Presentation 事件。
 */
#include "presentation_dispatch.h"

#include "esp_event.h"

ESP_EVENT_DEFINE_BASE(PRESENTATION_EVENT);

esp_err_t presentation_dispatch_page_switch(presentation_page_id_t page, presentation_nav_dir_t dir)
{
    if ((unsigned) page >= PRESENTATION_PAGE_COUNT || (unsigned) dir > PRESENTATION_NAV_DIR_BACKWARD)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const presentation_page_switch_event_t event = {
        .page = page,
        .dir  = dir,
    };
    return esp_event_post(PRESENTATION_EVENT, PRESENTATION_EVENT_PAGE_SWITCH, &event, sizeof(event), 0);
}

esp_err_t presentation_dispatch_status_bar_update(void)
{
    return esp_event_post(PRESENTATION_EVENT, PRESENTATION_EVENT_STATUS_BAR_UPDATE, NULL, 0, 0);
}

esp_err_t presentation_dispatch_status_update(void)
{
    return esp_event_post(PRESENTATION_EVENT, PRESENTATION_EVENT_STATUS_UPDATE, NULL, 0, 0);
}

esp_err_t presentation_dispatch_ota_update(void)
{
    return esp_event_post(PRESENTATION_EVENT, PRESENTATION_EVENT_OTA_UPDATE, NULL, 0, 0);
}

esp_err_t presentation_dispatch_settings_action(presentation_settings_action_t action)
{
    if ((unsigned) action > PRESENTATION_SETTINGS_ACTION_BACK)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const presentation_settings_action_event_t event = {
        .action = action,
    };
    return esp_event_post(PRESENTATION_EVENT, PRESENTATION_EVENT_SETTINGS_ACTION, &event, sizeof(event), 0);
}
