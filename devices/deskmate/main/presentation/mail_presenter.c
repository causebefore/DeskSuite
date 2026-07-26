/*
 * 文件职责：订阅邮件事实事件并维护邮箱页 View Model。
 */
#include "mail_presenter.h"

#include <stdio.h>
#include <string.h>

#include "presentation_dispatch.h"
#include "mail.h"

static mail_view_model_t s_view;

static void update_view_from_service(const mail_snapshot_t *data)
{
    if (data == NULL || !data->valid)
    {
        s_view.status = PRESENTATION_DATA_EMPTY;
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

    s_view.status = (s_view.message_count > 0) ? PRESENTATION_DATA_OK : PRESENTATION_DATA_EMPTY;
}

static void on_mail_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) data;

    mail_snapshot_t svc;
    if (mail_get_snapshot(&svc) == ESP_OK)
    {
        update_view_from_service(&svc);
    }

    (void) presentation_dispatch_status_update();
}

esp_err_t mail_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.status = PRESENTATION_DATA_EMPTY;

    mail_snapshot_t svc;
    if (mail_get_snapshot(&svc) == ESP_OK)
    {
        update_view_from_service(&svc);
    }

    return esp_event_handler_register(MAIL_EVENT, ESP_EVENT_ANY_ID, on_mail_event, NULL);
}

void mail_presenter_get_view_copy(mail_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
