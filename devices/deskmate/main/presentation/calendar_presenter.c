/*
 * 文件职责：从 Dashboard Store 刷新并维护日历页 View Model。
 */
#include "calendar_presenter.h"

#include <stdio.h>
#include <string.h>

#include "dashboard_store.h"

static calendar_view_model_t s_view;

/**
 * @brief 从 Dashboard Store 日历数据更新 View Model
 *
 * @param[in] data 日历数据
 */
static void update_view_from_store(const deskmate_api_dashboard_calendar_t *data)
{
    if (data == NULL || !data->valid)
    {
        memset(&s_view, 0, sizeof(s_view));
        s_view.status = PRESENTATION_DATA_ERROR;
        return;
    }

    memset(&s_view, 0, sizeof(s_view));
    snprintf(s_view.source, sizeof(s_view.source), "%s", data->source);

    s_view.event_count = data->event_count;
    if (s_view.event_count > CALENDAR_VIEW_EVENT_MAX)
    {
        s_view.event_count = CALENDAR_VIEW_EVENT_MAX;
    }

    for (uint8_t i = 0; i < s_view.event_count; ++i)
    {
        snprintf(s_view.events[i].title, sizeof(s_view.events[i].title), "%s", data->events[i].title);
        snprintf(s_view.events[i].relative, sizeof(s_view.events[i].relative), "%s", data->events[i].relative);
        s_view.events[i].all_day = data->events[i].all_day;
        snprintf(s_view.events[i].location, sizeof(s_view.events[i].location), "%s", data->events[i].location);
    }

    s_view.status = data->error[0] == '\0' ? PRESENTATION_DATA_OK : PRESENTATION_DATA_ERROR;
}

esp_err_t calendar_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.status = PRESENTATION_DATA_EMPTY;
    return ESP_OK;
}

esp_err_t calendar_presenter_refresh(void)
{
    deskmate_api_dashboard_calendar_t data;
    const esp_err_t                   error = dashboard_store_get_calendar_copy(&data);
    if (error != ESP_OK)
    {
        if (s_view.status == PRESENTATION_DATA_OK)
        {
            s_view.status = PRESENTATION_DATA_STALE;
        }
        else if (s_view.status == PRESENTATION_DATA_EMPTY)
        {
            s_view.status = PRESENTATION_DATA_ERROR;
        }
        return error;
    }
    update_view_from_store(&data);
    return ESP_OK;
}

void calendar_presenter_set_stale(void)
{
    if (s_view.status == PRESENTATION_DATA_OK)
    {
        s_view.status = PRESENTATION_DATA_STALE;
    }
}

void calendar_presenter_get_view_copy(calendar_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
