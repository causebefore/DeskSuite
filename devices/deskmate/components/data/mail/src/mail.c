/*
 * 文件职责：从 dashboard_store 读取邮件数据，维护邮件快照。
 * 主要依赖：dashboard_store、esp_event。
 * 调用方：app_network 和邮箱 Presenter。
 */
#include "mail.h"

#include <stdio.h>
#include <string.h>

#include "dashboard_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

ESP_EVENT_DEFINE_BASE(MAIL_EVENT);

static const char       *TAG = "mail";
static mail_snapshot_t   s_data;
static SemaphoreHandle_t s_lock;

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0)
    {
        return;
    }
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

/**
 * @brief 从 Dashboard Store 同步刷新邮件快照并发布结果事件
 */
esp_err_t mail_refresh_from_dashboard(void)
{
    dashboard_store_mail_t src;
    esp_err_t              err = dashboard_store_get_mail(&src);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "读取 dashboard 邮箱失败: %s", esp_err_to_name(err));
        (void) esp_event_post(MAIL_EVENT, MAIL_EVENT_FAILED, NULL, 0, 0);
        return err;
    }

    mail_snapshot_t next = { 0 };
    next.valid           = src.valid;
    next.unread_count    = src.unread_count;
    next.message_count   = src.message_count > MAIL_MESSAGE_MAX ? MAIL_MESSAGE_MAX : src.message_count;
    copy_text(next.source, sizeof(next.source), src.source);
    for (uint8_t i = 0; i < next.message_count; ++i)
    {
        copy_text(next.messages[i].from_name, sizeof(next.messages[i].from_name), src.messages[i].from_name);
        copy_text(next.messages[i].subject, sizeof(next.messages[i].subject), src.messages[i].subject);
        copy_text(next.messages[i].date_text, sizeof(next.messages[i].date_text), src.messages[i].date_text);
        next.messages[i].unread = src.messages[i].unread;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = next;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "邮箱刷新完成: count=%u unread=%u", (unsigned) next.message_count, (unsigned) next.unread_count);
    return esp_event_post(MAIL_EVENT, MAIL_EVENT_REFRESHED, NULL, 0, portMAX_DELAY);
}

esp_err_t mail_init(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_data, 0, sizeof(s_data));
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t mail_get_snapshot(mail_snapshot_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
