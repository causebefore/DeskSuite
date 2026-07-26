/*
 * 文件职责：订阅限额事实事件并维护限额页 View Model。
 */
#include "quota_presenter.h"

#include <stdio.h>
#include <string.h>

#include "presentation_dispatch.h"
#include "quota.h"

static quota_view_model_t s_view;

static void update_view_from_service(const quota_snapshot_t *data)
{
    if (data == NULL || !data->valid)
    {
        s_view.status = PRESENTATION_DATA_EMPTY;
        return;
    }

    memset(&s_view, 0, sizeof(s_view));
    snprintf(s_view.source, sizeof(s_view.source), "%s", data->source);
    snprintf(s_view.level, sizeof(s_view.level), "%s", data->level);
    snprintf(s_view.error, sizeof(s_view.error), "%s", data->error);
    snprintf(s_view.updated_at, sizeof(s_view.updated_at), "%s", data->updated_at);

    s_view.limit_count = data->limit_count;
    if (s_view.limit_count > QUOTA_VIEW_LIMIT_MAX)
    {
        s_view.limit_count = QUOTA_VIEW_LIMIT_MAX;
    }

    for (uint8_t i = 0; i < s_view.limit_count; ++i)
    {
        snprintf(s_view.limits[i].type, sizeof(s_view.limits[i].type), "%s", data->limits[i].type);
        s_view.limits[i].used_percent      = data->limits[i].used_percent;
        s_view.limits[i].remaining_percent = data->limits[i].remaining_percent;
        snprintf(s_view.limits[i].next_reset, sizeof(s_view.limits[i].next_reset), "%s", data->limits[i].next_reset);
    }

    /* spec §5.2 降级：!available → ERROR；available 且 limits 空 → EMPTY；否则 OK */
    if (!data->available)
    {
        s_view.status = PRESENTATION_DATA_ERROR;
    }
    else if (s_view.limit_count > 0)
    {
        s_view.status = PRESENTATION_DATA_OK;
    }
    else
    {
        s_view.status = PRESENTATION_DATA_EMPTY;
    }
}

static void on_quota_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) id;
    (void) data;

    quota_snapshot_t svc;
    if (quota_get_snapshot(&svc) == ESP_OK)
    {
        update_view_from_service(&svc);
    }

    (void) presentation_dispatch_status_update();
}

esp_err_t quota_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.status = PRESENTATION_DATA_EMPTY;

    quota_snapshot_t svc;
    if (quota_get_snapshot(&svc) == ESP_OK)
    {
        update_view_from_service(&svc);
    }

    return esp_event_handler_register(QUOTA_EVENT, ESP_EVENT_ANY_ID, on_quota_event, NULL);
}

void quota_presenter_get_view_copy(quota_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
