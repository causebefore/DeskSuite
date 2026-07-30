/**
 * @file deskmate_api.c
 * @brief 实现 DeskMate Dashboard schema 3 HTTP 协议
 */
#include "deskmate_api.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "protocol_identity.h"
#include "protocol_url.h"
#include "transport_http.h"

static const char *TAG = "deskmate_api";

/** @brief 按名称读取 JSON 对象字段 */
static const cJSON *json_obj_get(const cJSON *object, const char *name)
{
    if (object == NULL || name == NULL)
    {
        return NULL;
    }
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

/** @brief 从 JSON 对象复制可选字符串字段 */
static void json_copy_string(const cJSON *object, const char *name, char *out, size_t out_len)
{
    const cJSON *item = json_obj_get(object, name);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        (void) snprintf(out, out_len, "%s", item->valuestring);
    }
}

/** @brief 读取 JSON 数字并转换为整数 */
static bool json_get_int(const cJSON *object, const char *name, int *out)
{
    const cJSON *item = json_obj_get(object, name);
    if (!cJSON_IsNumber(item) || out == NULL)
    {
        return false;
    }
    *out = (int) item->valuedouble;
    return true;
}

/** @brief 读取 JSON 浮点数 */
static bool json_get_double(const cJSON *object, const char *name, double *out)
{
    const cJSON *item = json_obj_get(object, name);
    if (!cJSON_IsNumber(item) || out == NULL)
    {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

/** @brief 解析 Dashboard 天气块 */
static void parse_dashboard_weather(const cJSON *root, deskmate_api_dashboard_weather_t *out)
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
    const int count = total > DESKMATE_API_DASHBOARD_DAILY_MAX ? DESKMATE_API_DASHBOARD_DAILY_MAX : total;
    for (int i = 0; i < count; ++i)
    {
        const cJSON *item = cJSON_GetArrayItem(daily, i);
        if (!cJSON_IsObject(item))
        {
            return;
        }
        deskmate_api_dashboard_weather_daily_t *day = &out->daily[out->daily_count];
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

/** @brief 解析 Dashboard 日历块 */
static void parse_dashboard_calendar(const cJSON *root, deskmate_api_dashboard_calendar_t *out)
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
        if (out->event_count >= DESKMATE_API_DASHBOARD_CALENDAR_MAX)
        {
            break;
        }
        if (!cJSON_IsObject(item))
        {
            return;
        }
        deskmate_api_dashboard_calendar_event_t *event = &out->events[out->event_count];
        json_copy_string(item, "title", event->title, sizeof(event->title));
        json_copy_string(item, "relative", event->relative, sizeof(event->relative));
        json_copy_string(item, "location", event->location, sizeof(event->location));
        event->all_day = cJSON_IsTrue(json_obj_get(item, "all_day"));
        out->event_count++;
    }
    out->valid = true;
}

/** @brief 解析 Dashboard 邮件块 */
static void parse_dashboard_mail(const cJSON *root, deskmate_api_dashboard_mail_t *out)
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
        if (out->message_count >= DESKMATE_API_DASHBOARD_MAIL_MAX)
        {
            break;
        }
        if (!cJSON_IsObject(item))
        {
            return;
        }
        deskmate_api_dashboard_mail_message_t *message = &out->messages[out->message_count];
        json_copy_string(item, "from_name", message->from_name, sizeof(message->from_name));
        json_copy_string(item, "subject", message->subject, sizeof(message->subject));
        json_copy_string(item, "date_text", message->date_text, sizeof(message->date_text));
        message->unread = cJSON_IsTrue(json_obj_get(item, "unread"));
        out->message_count++;
    }
    out->valid = true;
}

/** @brief 解析 Dashboard 限额块 */
static void parse_dashboard_quota(const cJSON *root, deskmate_api_dashboard_quota_t *out)
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
        if (out->limit_count >= DESKMATE_API_DASHBOARD_QUOTA_MAX)
        {
            break;
        }
        if (!cJSON_IsObject(item))
        {
            return;
        }
        deskmate_api_dashboard_quota_item_t *limit = &out->limits[out->limit_count];
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

/** @brief 执行一次带统一设备身份头的 DeskMate JSON GET 请求 */
static esp_err_t perform_json_get(const deskmate_api_client_t *client, const char *path, size_t max_response_bytes,
                                  transport_http_response_t *response)
{
    ESP_RETURN_ON_FALSE(client != NULL && protocol_backend_context_is_valid(client->backend) && path != NULL
                            && response != NULL && max_response_bytes > 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "DeskMate API 请求参数无效");

    char url[224] = { 0 };
    ESP_RETURN_ON_ERROR(protocol_url_build(url, sizeof(url), client->backend->base_url, path),
                        TAG,
                        "构造 DeskMate API 地址失败");

    char                    authorization[128] = { 0 };
    transport_http_header_t headers[3]         = {
        { .name = "Accept", .value = "application/json" },
    };
    size_t header_count = 1U;
    protocol_identity_add_headers(headers,
                                  &header_count,
                                  client->backend->token,
                                  client->backend->device_id,
                                  authorization,
                                  sizeof(authorization));

    const size_t response_limit =
        max_response_bytes < DESKMATE_API_DASHBOARD_MAX_BYTES ? max_response_bytes : DESKMATE_API_DASHBOARD_MAX_BYTES;
    const transport_http_request_t request = {
        .url                = url,
        .method             = TRANSPORT_HTTP_GET,
        .headers            = headers,
        .header_count       = header_count,
        .body               = NULL,
        .body_len           = 0U,
        .timeout_ms         = client->timeout_ms,
        .max_response_bytes = response_limit,
    };
    return transport_http_perform_borrow(&request, response);
}

esp_err_t deskmate_api_get_dashboard(const deskmate_api_client_t *client, size_t max_response_bytes,
                                     deskmate_api_dashboard_result_t *out, int *http_status)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Dashboard 输出为空");
    memset(out, 0, sizeof(*out));

    transport_http_response_t response = { 0 };
    esp_err_t                 error    = perform_json_get(client, "/api/v1/dashboard", max_response_bytes, &response);
    if (http_status != NULL)
    {
        *http_status = response.status_code;
    }
    if (error == ESP_OK && response.status_code != 200)
    {
        error = ESP_FAIL;
    }
    if (error == ESP_OK
        && (response.body == NULL || response.body_len > max_response_bytes
            || response.body_len > DESKMATE_API_DASHBOARD_MAX_BYTES))
    {
        error = ESP_ERR_INVALID_SIZE;
    }

    if (error == ESP_OK)
    {
        cJSON *root = cJSON_Parse(response.body);
        if (root == NULL)
        {
            error = ESP_ERR_INVALID_RESPONSE;
        }
        else
        {
            const cJSON *schema       = json_obj_get(root, "schema");
            const cJSON *next_refresh = json_obj_get(root, "next_refresh_at_utc");
            const bool   schema_valid = cJSON_IsNumber(schema) && schema->valuedouble == DESKMATE_API_DASHBOARD_SCHEMA;
            const bool   next_refresh_valid =
                cJSON_IsNumber(next_refresh) && next_refresh->valuedouble >= 1704067200.0
                && next_refresh->valuedouble <= 4102444799.0
                && (double) ((int64_t) next_refresh->valuedouble) == next_refresh->valuedouble;
            out->schema = schema_valid ? DESKMATE_API_DASHBOARD_SCHEMA : 0;
            json_copy_string(root, "device_id", out->device_id, sizeof(out->device_id));
            json_copy_string(root, "generated_at", out->generated_at, sizeof(out->generated_at));
            out->next_refresh_at_utc = next_refresh_valid ? (int64_t) next_refresh->valuedouble : 0;
            const bool top_valid     = schema_valid && client != NULL && client->backend != NULL
                                       && strcmp(out->device_id, client->backend->device_id) == 0
                                       && out->generated_at[0] != '\0' && next_refresh_valid;
            if (top_valid)
            {
                parse_dashboard_weather(root, &out->weather);
                parse_dashboard_calendar(root, &out->calendar);
                parse_dashboard_mail(root, &out->mail);
                parse_dashboard_quota(root, &out->quota);
            }
            out->valid = top_valid && out->weather.valid && out->calendar.valid && out->mail.valid && out->quota.valid;
            cJSON_Delete(root);
            if (!out->valid)
            {
                error = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }

    transport_http_response_release(&response);
    return error;
}
