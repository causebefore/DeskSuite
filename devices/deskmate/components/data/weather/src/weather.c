/*
 * 文件职责：从 dashboard_store 读取天气数据，维护天气快照。
 * 主要依赖：dashboard_store、esp_event。
 * 调用方：app_network 和天气 Presenter。
 */
#include "weather.h"

#include <string.h>

#include "dashboard_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char        *TAG = "weather";
static weather_snapshot_t s_weather;
static SemaphoreHandle_t  s_lock;

ESP_EVENT_DEFINE_BASE(WEATHER_EVENT);

static void copy_string(char *dst, size_t dst_len, const char *src)
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
    size_t len = strlen(src);
    if (len >= dst_len)
    {
        len = dst_len - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_from_dashboard(const dashboard_store_weather_t *src, weather_snapshot_t *out)
{
    if (src == NULL || out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->valid = src->valid;
    copy_string(out->source, sizeof(out->source), src->source);
    copy_string(out->updated_at, sizeof(out->updated_at), src->updated_at);
    copy_string(out->city, sizeof(out->city), src->city);
    copy_string(out->weather_text, sizeof(out->weather_text), src->text);
    copy_string(out->icon, sizeof(out->icon), src->icon);
    out->temp_c           = src->temp_c;
    out->feels_like_c     = src->feels_like_c;
    out->humidity_percent = src->humidity_percent;
    copy_string(out->wind_dir, sizeof(out->wind_dir), src->wind_dir);
    copy_string(out->wind_scale, sizeof(out->wind_scale), src->wind_scale);
    out->pressure_hpa = src->pressure_hpa;
    out->precip_mm    = src->precip_mm;
    out->vis_km       = src->vis_km;
    out->daily_count  = src->daily_count > 3 ? 3 : src->daily_count;
    for (uint8_t i = 0; i < out->daily_count; ++i)
    {
        copy_string(out->daily[i].fx_date, sizeof(out->daily[i].fx_date), src->daily[i].fx_date);
        copy_string(out->daily[i].text_day, sizeof(out->daily[i].text_day), src->daily[i].text_day);
        copy_string(out->daily[i].text_night, sizeof(out->daily[i].text_night), src->daily[i].text_night);
        copy_string(out->daily[i].icon_day, sizeof(out->daily[i].icon_day), src->daily[i].icon_day);
        out->daily[i].temp_min_c = src->daily[i].temp_min_c;
        out->daily[i].temp_max_c = src->daily[i].temp_max_c;
        copy_string(out->daily[i].sunrise, sizeof(out->daily[i].sunrise), src->daily[i].sunrise);
        copy_string(out->daily[i].sunset, sizeof(out->daily[i].sunset), src->daily[i].sunset);
    }
    out->aqi = src->aqi;
    copy_string(out->aqi_category, sizeof(out->aqi_category), src->aqi_category);
    copy_string(out->minutely_summary, sizeof(out->minutely_summary), src->minutely_summary);
    copy_string(out->alert_title, sizeof(out->alert_title), src->alert_title);
    copy_string(out->alert_severity, sizeof(out->alert_severity), src->alert_severity);
    copy_string(out->error, sizeof(out->error), src->error);
}

/**
 * @brief 从 Dashboard Store 同步刷新天气快照并发布结果事件
 */
esp_err_t weather_refresh_from_dashboard(void)
{
    dashboard_store_weather_t src;
    esp_err_t                 err = dashboard_store_get_weather(&src);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "读取 dashboard 天气失败: %s", esp_err_to_name(err));
        (void) esp_event_post(WEATHER_EVENT, WEATHER_EVENT_FAILED, NULL, 0, 0);
        return err;
    }

    weather_snapshot_t next;
    copy_from_dashboard(&src, &next);
    if (!next.valid)
    {
        if (next.error[0] == '\0')
        {
            copy_string(next.error, sizeof(next.error), "天气 JSON 字段不完整");
        }
        ESP_LOGW(TAG, "天气数据无效或不完整");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather = next;
    xSemaphoreGive(s_lock);

    if (next.valid)
    {
        ESP_LOGI(TAG,
                 "天气实况: %s %d°C(体感%d°C) %s 湿度%u%% %s风%s级",
                 next.city,
                 next.temp_c,
                 next.feels_like_c,
                 next.weather_text,
                 (unsigned) next.humidity_percent,
                 next.wind_dir,
                 next.wind_scale);
    }
    return esp_event_post(WEATHER_EVENT, WEATHER_EVENT_REFRESHED, NULL, 0, portMAX_DELAY);
}

esp_err_t weather_init(void)
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
    memset(&s_weather, 0, sizeof(s_weather));
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t weather_get_snapshot(weather_snapshot_t *out)
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
    *out = s_weather;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
