/*
 * 文件职责：从 dashboard_store 读取限额数据，维护限额快照。
 * 主要依赖：dashboard_store、esp_event。
 * 调用方：app_network 和限额 Presenter。
 */
#include "quota.h"

#include <stdio.h>
#include <string.h>

#include "dashboard_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

ESP_EVENT_DEFINE_BASE(QUOTA_EVENT);

static const char       *TAG = "quota";
static quota_snapshot_t  s_data;
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
 * @brief 从 Dashboard Store 同步刷新限额快照并发布结果事件
 */
esp_err_t quota_refresh_from_dashboard(void)
{
    dashboard_store_quota_t src;
    esp_err_t               err = dashboard_store_get_quota(&src);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "读取 dashboard 限额失败: %s", esp_err_to_name(err));
        (void) esp_event_post(QUOTA_EVENT, QUOTA_EVENT_FAILED, NULL, 0, 0);
        return err;
    }

    quota_snapshot_t next = { 0 };
    next.valid            = src.valid;
    next.available        = src.available;
    next.limit_count      = src.limit_count > QUOTA_LIMIT_MAX ? QUOTA_LIMIT_MAX : src.limit_count;
    copy_text(next.source, sizeof(next.source), src.source);
    copy_text(next.level, sizeof(next.level), src.level);
    copy_text(next.error, sizeof(next.error), src.error);
    copy_text(next.updated_at, sizeof(next.updated_at), src.updated_at);
    for (uint8_t i = 0; i < next.limit_count; ++i)
    {
        copy_text(next.limits[i].type, sizeof(next.limits[i].type), src.limits[i].type);
        next.limits[i].used_percent      = src.limits[i].used_percent;
        next.limits[i].remaining_percent = src.limits[i].remaining_percent;
        copy_text(next.limits[i].next_reset, sizeof(next.limits[i].next_reset), src.limits[i].next_reset);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = next;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "限额刷新完成: available=%d count=%u", (int) next.available, (unsigned) next.limit_count);
    return esp_event_post(QUOTA_EVENT, QUOTA_EVENT_REFRESHED, NULL, 0, portMAX_DELAY);
}

esp_err_t quota_init(void)
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

esp_err_t quota_get_snapshot(quota_snapshot_t *out)
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
