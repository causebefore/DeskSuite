/*
 * 文件职责：订阅天气事实事件并维护天气页 View Model。
 */
#include "weather_presenter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "presentation_dispatch.h"
#include "esp_event.h"
#include "esp_log.h"
#include "weather.h"

static weather_view_model_t s_view;

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

static uint16_t icon_code_from_text(const char *text)
{
    if (text == NULL || text[0] == '\0')
    {
        return 999;
    }
    return (uint16_t) atoi(text);
}

static void update_weather_view(const weather_snapshot_t *weather, esp_err_t result)
{
    if (weather == NULL || result != ESP_OK || !weather->valid)
    {
        s_view.status = result == ESP_OK ? PRESENTATION_DATA_EMPTY : PRESENTATION_DATA_ERROR;
        return;
    }

    memset(&s_view, 0, sizeof(s_view));
    copy_text(s_view.city, sizeof(s_view.city), weather->city);
    copy_text(s_view.text, sizeof(s_view.text), weather->weather_text);
    copy_text(s_view.wind_scale, sizeof(s_view.wind_scale), weather->wind_scale);
    copy_text(s_view.alert_title, sizeof(s_view.alert_title), weather->alert_title);
    copy_text(s_view.updated_at, sizeof(s_view.updated_at), weather->updated_at);
    s_view.code         = icon_code_from_text(weather->icon);
    s_view.temp_c       = weather->temp_c;
    s_view.feels_like_c = weather->feels_like_c;
    s_view.humidity     = weather->humidity_percent;
    s_view.pressure_hpa = weather->pressure_hpa;
    s_view.precip_mm    = weather->precip_mm;
    s_view.vis_km       = weather->vis_km;
    s_view.daily_count  = weather->daily_count > 3 ? 3 : weather->daily_count;
    s_view.aqi          = weather->aqi;
    copy_text(s_view.aqi_category, sizeof(s_view.aqi_category), weather->aqi_category);
    s_view.status = PRESENTATION_DATA_OK;

    for (uint8_t i = 0; i < s_view.daily_count; ++i)
    {
        copy_text(s_view.daily[i].fx_date, sizeof(s_view.daily[i].fx_date), weather->daily[i].fx_date);
        copy_text(s_view.daily[i].text_day, sizeof(s_view.daily[i].text_day), weather->daily[i].text_day);
        s_view.daily[i].icon_day   = icon_code_from_text(weather->daily[i].icon_day);
        s_view.daily[i].temp_min_c = weather->daily[i].temp_min_c;
        s_view.daily[i].temp_max_c = weather->daily[i].temp_max_c;
        copy_text(s_view.daily[i].sunrise, sizeof(s_view.daily[i].sunrise), weather->daily[i].sunrise);
        copy_text(s_view.daily[i].sunset, sizeof(s_view.daily[i].sunset), weather->daily[i].sunset);
    }
}

static void on_weather_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) data;

    weather_snapshot_t weather;
    esp_err_t          err = weather_get_snapshot(&weather);

    esp_err_t result       = (id == WEATHER_EVENT_REFRESHED && err == ESP_OK) ? ESP_OK : ESP_FAIL;
    update_weather_view(&weather, result);
    (void) presentation_dispatch_status_update();
}

esp_err_t weather_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.status = PRESENTATION_DATA_EMPTY;
    return esp_event_handler_register(WEATHER_EVENT, ESP_EVENT_ANY_ID, on_weather_event, NULL);
}

void weather_presenter_get_view_copy(weather_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
