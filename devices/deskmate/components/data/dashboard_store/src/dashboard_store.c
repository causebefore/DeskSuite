/*
 * 文件职责：缓存协议层已校验的 Dashboard 类型化结果，并提供线程安全切片读取。
 */
#include "dashboard_store.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static deskmate_api_dashboard_result_t s_snapshot;
static SemaphoreHandle_t               s_lock;

esp_err_t dashboard_store_init(void)
{
    if (s_lock != NULL)
    {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t dashboard_store_update_copy(const deskmate_api_dashboard_result_t *dashboard)
{
    if (dashboard == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dashboard->valid)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const esp_err_t error = dashboard_store_init();
    if (error != ESP_OK)
    {
        return error;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot = *dashboard;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dashboard_store_get_weather_copy(deskmate_api_dashboard_weather_t *out)
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
    *out = s_snapshot.weather;
    xSemaphoreGive(s_lock);
    return out->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t dashboard_store_get_calendar_copy(deskmate_api_dashboard_calendar_t *out)
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
    *out = s_snapshot.calendar;
    xSemaphoreGive(s_lock);
    return out->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t dashboard_store_get_mail_copy(deskmate_api_dashboard_mail_t *out)
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
    *out = s_snapshot.mail;
    xSemaphoreGive(s_lock);
    return out->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t dashboard_store_get_quota_copy(deskmate_api_dashboard_quota_t *out)
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
    *out = s_snapshot.quota;
    xSemaphoreGive(s_lock);
    return out->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
