/*
 * 文件职责：从 dashboard_store 读取日程数据，维护日程快照。
 * 主要依赖：dashboard_store、esp_event。
 * 调用方：app_network 和日历 Presenter。
 */
#include "calendar.h"

#include <stdio.h>
#include <string.h>

#include "dashboard_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

ESP_EVENT_DEFINE_BASE(CALENDAR_DATA_EVENT);

static const char         *TAG = "calendar";
static calendar_snapshot_t s_data;
static SemaphoreHandle_t   s_lock;

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
 * @brief 从 Dashboard Store 同步刷新日历快照并发布结果事件
 */
esp_err_t calendar_refresh_from_dashboard(void)
{
    dashboard_store_calendar_t src;
    esp_err_t                  err = dashboard_store_get_calendar(&src);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "读取 dashboard 日程失败: %s", esp_err_to_name(err));
        (void) esp_event_post(CALENDAR_DATA_EVENT, CALENDAR_DATA_EVENT_FAILED, NULL, 0, 0);
        return err;
    }

    calendar_snapshot_t next = { 0 };
    next.valid               = src.valid;
    copy_text(next.source, sizeof(next.source), src.source);
    next.event_count = src.event_count > CALENDAR_EVENT_MAX ? CALENDAR_EVENT_MAX : src.event_count;
    for (uint8_t i = 0; i < next.event_count; ++i)
    {
        copy_text(next.events[i].title, sizeof(next.events[i].title), src.events[i].title);
        copy_text(next.events[i].relative, sizeof(next.events[i].relative), src.events[i].relative);
        next.events[i].all_day = src.events[i].all_day;
        copy_text(next.events[i].location, sizeof(next.events[i].location), src.events[i].location);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = next;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "日程刷新完成: count=%u", (unsigned) next.event_count);
    return esp_event_post(CALENDAR_DATA_EVENT, CALENDAR_DATA_EVENT_REFRESHED, NULL, 0, portMAX_DELAY);
}

esp_err_t calendar_init(void)
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

esp_err_t calendar_get_snapshot(calendar_snapshot_t *out)
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
