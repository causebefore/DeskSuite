#include "log_upload.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "protocol_url.h"
#include "transport.h"

static const char *TAG = "log_upload";

static void copy_session(const char *json, char *out, size_t out_len)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (cJSON_IsString(session) && session->valuestring != NULL)
    {
        snprintf(out, out_len, "%s", session->valuestring);
    }
    cJSON_Delete(root);
}

static esp_err_t post_json(const char *base_url,
                           const char *path,
                           int timeout_ms,
                           const char *body,
                           transport_http_response_t *response)
{
    char url[192] = { 0 };
    ESP_RETURN_ON_ERROR(protocol_url_build(url, sizeof(url), base_url, path),
                        TAG,
                        "构造日志上传 URL 失败");
    const transport_http_header_t headers[] = {
        { .name = "Content-Type", .value = "application/json" },
    };
    const transport_http_request_t request = {
        .url = url,
        .method = TRANSPORT_HTTP_POST,
        .headers = headers,
        .header_count = 1,
        .body = body,
        .body_len = strlen(body),
        .timeout_ms = timeout_ms,
        .max_response_bytes = 256,
    };
    esp_err_t err = transport_http_perform_borrow(&request, response);
    if (err == ESP_OK && (response->status_code < 200 || response->status_code >= 300))
    {
        err = ESP_FAIL;
    }
    return err;
}

esp_err_t log_upload_start(const char *base_url,
                                    int timeout_ms,
                                    const log_upload_boot_t *boot,
                                    char *session_id,
                                    size_t session_id_len)
{
    ESP_RETURN_ON_FALSE(base_url != NULL && boot != NULL && boot->product_id > 0U && session_id != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "日志启动参数无效");
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "创建日志启动 JSON 失败");
    cJSON_AddNumberToObject(root, "product_id", (double) boot->product_id);
    cJSON_AddStringToObject(root, "device_id", boot->device_id);
    cJSON_AddStringToObject(root, "firmware_version", boot->firmware_version);
    cJSON_AddStringToObject(root, "reset_reason", boot->reset_reason);
    cJSON_AddStringToObject(root, "ip", boot->ip != NULL ? boot->ip : "");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "序列化日志启动 JSON 失败");
    transport_http_response_t response = { 0 };
    esp_err_t err = post_json(base_url, "api/v1/logs/boot", timeout_ms, body, &response);
    cJSON_free(body);
    if (err == ESP_OK)
    {
        copy_session(response.body, session_id, session_id_len);
        if (session_id[0] == '\0')
        {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    transport_http_response_release(&response);
    return err;
}

esp_err_t log_upload_batch(const char *base_url,
                                    int timeout_ms,
                                    uint32_t product_id,
                                    const char *device_id,
                                    const char *session_id,
                                    const log_upload_line_t *lines,
                                    size_t count,
                                    char *next_session_id,
                                    size_t next_session_id_len)
{
    ESP_RETURN_ON_FALSE(base_url != NULL && product_id > 0U && device_id != NULL && lines != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "日志批次参数无效");
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "创建日志批次 JSON 失败");
    if (session_id != NULL && session_id[0] != '\0')
    {
        cJSON_AddStringToObject(root, "session_id", session_id);
    }
    else
    {
        cJSON_AddNullToObject(root, "session_id");
    }
    cJSON_AddNumberToObject(root, "product_id", (double) product_id);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON *array = cJSON_AddArrayToObject(root, "lines");
    for (size_t i = 0; array != NULL && i < count; ++i)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seq", (double) lines[i].seq);
        cJSON_AddNumberToObject(item, "uptime_ms", (double) lines[i].uptime_ms);
        cJSON_AddStringToObject(item, "level", lines[i].level);
        cJSON_AddStringToObject(item, "tag", lines[i].tag);
        cJSON_AddStringToObject(item, "message", lines[i].message);
        cJSON_AddStringToObject(item, "raw", lines[i].raw);
        cJSON_AddItemToArray(array, item);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "序列化日志批次 JSON 失败");
    transport_http_response_t response = { 0 };
    esp_err_t err = post_json(base_url, "api/v1/logs/batch", timeout_ms, body, &response);
    cJSON_free(body);
    if (err == ESP_OK && next_session_id != NULL && next_session_id_len > 0)
    {
        copy_session(response.body, next_session_id, next_session_id_len);
    }
    transport_http_response_release(&response);
    return err;
}
