/**
 * @file deskmate_api.c
 * @brief 实现 DeskMate Dashboard schema 3 HTTP 协议
 */
#include "deskmate_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "protocol_identity.h"
#include "protocol_url.h"
#include "transport_http.h"

static const char *TAG = "deskmate_api";

/** @brief 从 JSON 对象复制可选字符串字段 */
static void json_copy_string(const cJSON *object, const char *name, char *out, size_t out_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        (void) snprintf(out, out_len, "%s", item->valuestring);
    }
}

/** @brief 执行一次带统一设备身份头的 DeskMate JSON GET 请求 */
static esp_err_t perform_json_get(const deskmate_api_client_t *client, const char *path, size_t max_response_bytes,
                                  transport_http_response_t *response)
{
    ESP_RETURN_ON_FALSE(client != NULL && client->base_url != NULL && client->base_url[0] != '\0'
                            && client->device_id != NULL && client->device_id[0] != '\0' && path != NULL
                            && response != NULL && max_response_bytes > 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "DeskMate API 请求参数无效");

    char url[224] = { 0 };
    ESP_RETURN_ON_ERROR(protocol_url_build(url, sizeof(url), client->base_url, path),
                        TAG,
                        "构造 DeskMate API 地址失败");

    char                    authorization[128] = { 0 };
    transport_http_header_t headers[3]         = {
        { .name = "Accept", .value = "application/json" },
    };
    size_t header_count = 1U;
    protocol_identity_add_headers(headers,
                                  &header_count,
                                  client->device_token,
                                  client->device_id,
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
                                     deskmate_api_dashboard_t *out, int *http_status)
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
            const cJSON *schema       = cJSON_GetObjectItemCaseSensitive(root, "schema");
            const cJSON *next_refresh = cJSON_GetObjectItemCaseSensitive(root, "next_refresh_at_utc");
            const bool   schema_valid = cJSON_IsNumber(schema) && schema->valuedouble == DESKMATE_API_DASHBOARD_SCHEMA;
            const bool   next_refresh_valid =
                cJSON_IsNumber(next_refresh) && next_refresh->valuedouble >= 1704067200.0
                && next_refresh->valuedouble <= 4102444799.0
                && (double) ((int64_t) next_refresh->valuedouble) == next_refresh->valuedouble;
            out->schema = schema_valid ? DESKMATE_API_DASHBOARD_SCHEMA : 0;
            json_copy_string(root, "device_id", out->device_id, sizeof(out->device_id));
            json_copy_string(root, "generated_at", out->generated_at, sizeof(out->generated_at));
            out->next_refresh_at_utc = next_refresh_valid ? (int64_t) next_refresh->valuedouble : 0;
            out->valid               = schema_valid && client != NULL && client->device_id != NULL
                                       && strcmp(out->device_id, client->device_id) == 0 && out->generated_at[0] != '\0'
                                       && next_refresh_valid;
            cJSON_Delete(root);
            if (!out->valid)
            {
                error = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }

    if (error == ESP_OK)
    {
        out->raw_json     = response.body;
        out->raw_json_len = response.body_len;
        response.body     = NULL;
    }
    transport_http_response_release(&response);
    return error;
}

void deskmate_api_dashboard_release(deskmate_api_dashboard_t *dashboard)
{
    if (dashboard == NULL)
    {
        return;
    }
    free(dashboard->raw_json);
    memset(dashboard, 0, sizeof(*dashboard));
}
