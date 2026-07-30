/*
 * 文件职责：从 Dashboard Store 刷新并维护限额页 View Model。
 */
#include "quota_presenter.h"

#include <stdio.h>
#include <string.h>

#include "dashboard_store.h"

static quota_view_model_t s_view;

static void update_view_from_store(const deskmate_api_dashboard_quota_t *data)
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

esp_err_t quota_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.status = PRESENTATION_DATA_EMPTY;
    return ESP_OK;
}

esp_err_t quota_presenter_refresh(void)
{
    deskmate_api_dashboard_quota_t data;
    const esp_err_t                error = dashboard_store_get_quota_copy(&data);
    if (error != ESP_OK)
    {
        return error;
    }
    update_view_from_store(&data);
    return ESP_OK;
}

void quota_presenter_get_view_copy(quota_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
