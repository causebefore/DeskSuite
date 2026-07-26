/*
 * 文件职责：订阅日历事实事件并维护日历页 View Model。
 */
#include "calendar_presenter.h"

#include <stdio.h>
#include <string.h>

#include "presentation_dispatch.h"
#include "calendar.h"

static calendar_view_model_t s_view;

/** 
 * @brief 更新视图数据从日历服务快照
 * 
 * @param data 日历服务快照数据
 */
static void update_view_from_service(const calendar_snapshot_t *data)
{
    if (data == NULL || !data->valid)
    {
        s_view.status = PRESENTATION_DATA_EMPTY;
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

    s_view.status = (s_view.event_count > 0) ? PRESENTATION_DATA_OK : PRESENTATION_DATA_EMPTY;
}

static void on_calendar_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) data;

    calendar_snapshot_t svc;
    if (calendar_get_snapshot(&svc) == ESP_OK)
    {
        update_view_from_service(&svc);
    }

    (void) presentation_dispatch_status_update();
}

esp_err_t calendar_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.status = PRESENTATION_DATA_EMPTY;

    calendar_snapshot_t svc;
    if (calendar_get_snapshot(&svc) == ESP_OK)
    {
        update_view_from_service(&svc);
    }

    return esp_event_handler_register(CALENDAR_DATA_EVENT, ESP_EVENT_ANY_ID, on_calendar_event, NULL);
}

void calendar_presenter_get_view_copy(calendar_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
