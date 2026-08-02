/*
 * 文件职责：从 Dashboard Store 刷新并维护邮箱页 View Model。
 */
#include "mail_presenter.h"

#include <stdio.h>
#include <string.h>

#include "dashboard_store.h"

static mail_view_model_t s_view;

static void update_view_from_store(const deskmate_api_dashboard_mail_t *data)
{
    if (data == NULL || !data->valid)
    {
        memset(&s_view, 0, sizeof(s_view));
        s_view.status = PRESENTATION_DATA_ERROR;
        return;
    }

    memset(&s_view, 0, sizeof(s_view));
    snprintf(s_view.source, sizeof(s_view.source), "%s", data->source);
    s_view.unread_count  = data->unread_count;

    s_view.message_count = data->message_count;
    if (s_view.message_count > MAIL_VIEW_MESSAGE_MAX)
    {
        s_view.message_count = MAIL_VIEW_MESSAGE_MAX;
    }

    for (uint8_t i = 0; i < s_view.message_count; ++i)
    {
        snprintf(s_view.messages[i].from_name, sizeof(s_view.messages[i].from_name), "%s", data->messages[i].from_name);
        snprintf(s_view.messages[i].subject, sizeof(s_view.messages[i].subject), "%s", data->messages[i].subject);
        snprintf(s_view.messages[i].date_text, sizeof(s_view.messages[i].date_text), "%s", data->messages[i].date_text);
        s_view.messages[i].unread = data->messages[i].unread;
    }

    s_view.status = data->error[0] == '\0' ? PRESENTATION_DATA_OK : PRESENTATION_DATA_ERROR;
}

esp_err_t mail_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.status = PRESENTATION_DATA_EMPTY;
    return ESP_OK;
}

esp_err_t mail_presenter_refresh(void)
{
    deskmate_api_dashboard_mail_t data;
    const esp_err_t               error = dashboard_store_get_mail_copy(&data);
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

void mail_presenter_set_stale(void)
{
    if (s_view.status == PRESENTATION_DATA_OK)
    {
        s_view.status = PRESENTATION_DATA_STALE;
    }
}

void mail_presenter_get_view_copy(mail_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
