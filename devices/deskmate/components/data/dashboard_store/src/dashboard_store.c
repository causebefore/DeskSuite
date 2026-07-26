/*
 * 文件职责：解析 Dashboard schema 3 JSON，并提供线程安全快照读取。
 */
#include "dashboard_store.h"

#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static dashboard_store_snapshot_t s_snapshot;
static SemaphoreHandle_t          s_lock;

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

static const cJSON *json_obj_get(const cJSON *object, const char *name)
{
    if (object == NULL || name == NULL)
    {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static void json_copy_string(const cJSON *object, const char *name, char *out, size_t out_len)
{
    const cJSON *item = json_obj_get(object, name);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        copy_string(out, out_len, item->valuestring);
    }
}

static bool json_get_int(const cJSON *object, const char *name, int *out)
{
    const cJSON *item = json_obj_get(object, name);
    if (!cJSON_IsNumber(item))
    {
        return false;
    }
    *out = (int) item->valuedouble;
    return true;
}

static bool json_get_double(const cJSON *object, const char *name, double *out)
{
    const cJSON *item = json_obj_get(object, name);
    if (!cJSON_IsNumber(item))
    {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static void parse_weather(const cJSON *root, dashboard_store_weather_t *out)
{
    const cJSON *weather = json_obj_get(root, "weather");
    if (!cJSON_IsObject(weather) || out == NULL)
    {
        return;
    }

    int    value = 0;
    double dval  = 0.0;

    json_copy_string(weather, "source", out->source, sizeof(out->source));
    json_copy_string(weather, "updated_at", out->updated_at, sizeof(out->updated_at));
    json_copy_string(weather, "error", out->error, sizeof(out->error));
    json_copy_string(weather, "city", out->city, sizeof(out->city));
    json_copy_string(weather, "text", out->text, sizeof(out->text));
    json_copy_string(weather, "icon", out->icon, sizeof(out->icon));
    json_copy_string(weather, "wind_dir", out->wind_dir, sizeof(out->wind_dir));
    json_copy_string(weather, "wind_scale", out->wind_scale, sizeof(out->wind_scale));
    json_copy_string(weather, "minutely_summary", out->minutely_summary, sizeof(out->minutely_summary));
    json_copy_string(weather, "alert_title", out->alert_title, sizeof(out->alert_title));
    json_copy_string(weather, "alert_severity", out->alert_severity, sizeof(out->alert_severity));
    json_copy_string(weather, "aqi_category", out->aqi_category, sizeof(out->aqi_category));

    if (json_get_int(weather, "temp_c", &value))
    {
        out->temp_c = value;
    }
    if (json_get_int(weather, "feels_like_c", &value))
    {
        out->feels_like_c = value;
    }
    if (json_get_int(weather, "humidity_percent", &value) && value >= 0 && value <= 100)
    {
        out->humidity_percent = (uint8_t) value;
    }
    if (json_get_int(weather, "pressure_hpa", &value) && value > 0)
    {
        out->pressure_hpa = value;
    }
    if (json_get_double(weather, "precip_mm", &dval) && dval >= 0.0)
    {
        out->precip_mm = (float) dval;
    }
    if (json_get_int(weather, "vis_km", &value) && value >= 0)
    {
        out->vis_km = value;
    }
    if (json_get_int(weather, "aqi", &value) && value > 0)
    {
        out->aqi = (uint16_t) value;
    }

    const cJSON *daily = json_obj_get(weather, "daily");
    if (!cJSON_IsArray(daily))
    {
        return;
    }
    const int total = cJSON_GetArraySize(daily);
    const int count = total > DASHBOARD_STORE_DAILY_MAX ? DASHBOARD_STORE_DAILY_MAX : total;
    for (int i = 0; i < count; ++i)
    {
        const cJSON *item = cJSON_GetArrayItem(daily, i);
        if (!cJSON_IsObject(item))
        {
            return;
        }
        dashboard_store_weather_daily_t *day = &out->daily[out->daily_count];
        json_copy_string(item, "fx_date", day->fx_date, sizeof(day->fx_date));
        json_copy_string(item, "text_day", day->text_day, sizeof(day->text_day));
        json_copy_string(item, "text_night", day->text_night, sizeof(day->text_night));
        json_copy_string(item, "icon_day", day->icon_day, sizeof(day->icon_day));
        json_copy_string(item, "sunrise", day->sunrise, sizeof(day->sunrise));
        json_copy_string(item, "sunset", day->sunset, sizeof(day->sunset));
        if (json_get_int(item, "temp_min_c", &value))
        {
            day->temp_min_c = value;
        }
        if (json_get_int(item, "temp_max_c", &value))
        {
            day->temp_max_c = value;
        }
        out->daily_count++;
    }

    out->valid = true;
}

static void parse_calendar(const cJSON *root, dashboard_store_calendar_t *out)
{
    const cJSON *calendar = json_obj_get(root, "calendar");
    if (!cJSON_IsObject(calendar) || out == NULL)
    {
        return;
    }

    json_copy_string(calendar, "source", out->source, sizeof(out->source));
    json_copy_string(calendar, "error", out->error, sizeof(out->error));

    const cJSON *items = json_obj_get(calendar, "items");
    if (!cJSON_IsArray(items))
    {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items)
    {
        if (out->event_count >= DASHBOARD_STORE_CALENDAR_MAX)
        {
            break;
        }
        if (!cJSON_IsObject(item))
        {
            return;
        }
        dashboard_store_calendar_event_t *event = &out->events[out->event_count];
        json_copy_string(item, "title", event->title, sizeof(event->title));
        json_copy_string(item, "relative", event->relative, sizeof(event->relative));
        json_copy_string(item, "location", event->location, sizeof(event->location));
        event->all_day = cJSON_IsTrue(json_obj_get(item, "all_day"));
        out->event_count++;
    }
    out->valid = true;
}

static void parse_mail(const cJSON *root, dashboard_store_mail_t *out)
{
    const cJSON *mail = json_obj_get(root, "mail");
    if (!cJSON_IsObject(mail) || out == NULL)
    {
        return;
    }

    int value = 0;
    json_copy_string(mail, "source", out->source, sizeof(out->source));
    json_copy_string(mail, "error", out->error, sizeof(out->error));
    if (json_get_int(mail, "unread_count", &value) && value >= 0)
    {
        out->unread_count = (uint8_t) (value > 255 ? 255 : value);
    }

    const cJSON *messages = json_obj_get(mail, "messages");
    if (!cJSON_IsArray(messages))
    {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, messages)
    {
        if (out->message_count >= DASHBOARD_STORE_MAIL_MAX)
        {
            break;
        }
        if (!cJSON_IsObject(item))
        {
            return;
        }
        dashboard_store_mail_message_t *message = &out->messages[out->message_count];
        json_copy_string(item, "from_name", message->from_name, sizeof(message->from_name));
        json_copy_string(item, "subject", message->subject, sizeof(message->subject));
        json_copy_string(item, "date_text", message->date_text, sizeof(message->date_text));
        message->unread = cJSON_IsTrue(json_obj_get(item, "unread"));
        out->message_count++;
    }
    out->valid = true;
}

static void parse_quota(const cJSON *root, dashboard_store_quota_t *out)
{
    const cJSON *quota = json_obj_get(root, "quota");
    if (!cJSON_IsObject(quota) || out == NULL)
    {
        return;
    }

    const cJSON *available = json_obj_get(quota, "available");
    const cJSON *limits    = json_obj_get(quota, "limits");
    if (!cJSON_IsBool(available) || !cJSON_IsArray(limits))
    {
        return;
    }
    out->available = cJSON_IsTrue(available);
    json_copy_string(quota, "source", out->source, sizeof(out->source));
    json_copy_string(quota, "level", out->level, sizeof(out->level));
    json_copy_string(quota, "error", out->error, sizeof(out->error));
    json_copy_string(quota, "updated_at", out->updated_at, sizeof(out->updated_at));

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, limits)
    {
        if (out->limit_count >= DASHBOARD_STORE_QUOTA_MAX)
        {
            break;
        }
        if (!cJSON_IsObject(item))
        {
            return;
        }
        dashboard_store_quota_item_t *limit = &out->limits[out->limit_count];
        json_copy_string(item, "type", limit->type, sizeof(limit->type));
        double dval = 0.0;
        if (json_get_double(item, "used_percent", &dval))
        {
            limit->used_percent = (float) dval;
        }
        if (json_get_double(item, "remaining_percent", &dval))
        {
            limit->remaining_percent = (float) dval;
        }
        json_copy_string(item, "next_reset", limit->next_reset, sizeof(limit->next_reset));
        out->limit_count++;
    }
    out->valid = true;
}

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

esp_err_t dashboard_store_update_from_json(const char *json)
{
    if (json == NULL || json[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = dashboard_store_init();
    if (err != ESP_OK)
    {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    dashboard_store_snapshot_t next        = { 0 };
    const cJSON               *schema_item = json_obj_get(root, "schema");
    if (cJSON_IsNumber(schema_item) && schema_item->valuedouble == 3.0)
    {
        next.schema = 3;
    }
    json_copy_string(root, "device_id", next.device_id, sizeof(next.device_id));
    json_copy_string(root, "generated_at", next.generated_at, sizeof(next.generated_at));
    parse_weather(root, &next.weather);
    parse_calendar(root, &next.calendar);
    parse_mail(root, &next.mail);
    parse_quota(root, &next.quota);
    next.valid = next.schema == 3 && next.device_id[0] != '\0' && next.generated_at[0] != '\0' && next.weather.valid
                 && next.calendar.valid && next.mail.valid && next.quota.valid;

    cJSON_Delete(root);
    if (!next.valid)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot = next;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dashboard_store_get_snapshot(dashboard_store_snapshot_t *out)
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
    *out = s_snapshot;
    xSemaphoreGive(s_lock);
    return out->valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t dashboard_store_get_weather(dashboard_store_weather_t *out)
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

esp_err_t dashboard_store_get_calendar(dashboard_store_calendar_t *out)
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

esp_err_t dashboard_store_get_mail(dashboard_store_mail_t *out)
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

esp_err_t dashboard_store_get_quota(dashboard_store_quota_t *out)
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
