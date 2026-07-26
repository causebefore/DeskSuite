/**
 * @file device_status_protocol.c
 * @brief 实现设备温湿度与电池状态上传协议
 */
#include "device_status_protocol.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "protocol_identity.h"
#include "protocol_url.h"
#include "transport.h"

/** @brief 日志标签 */
static const char *TAG = "device_status";

/** @brief 判断上传数值是否符合服务端新契约 */
static bool device_status_protocol_values_are_valid(const device_status_protocol_upload_t *status)
{
    if (status == NULL || !isfinite(status->battery_percent) || status->battery_percent < 0.0F
        || status->battery_percent > 100.0F || status->battery_voltage_mv > 6000U)
    {
        return false;
    }
    return !status->has_environment
           || (isfinite(status->temperature_c) && status->temperature_c >= -50.0F
               && status->temperature_c <= 100.0F && isfinite(status->humidity_percent)
               && status->humidity_percent >= 0.0F && status->humidity_percent <= 100.0F);
}

esp_err_t device_status_protocol_upload_borrow(const char *in_base_url, const char *in_token,
                                               const char                            *in_device_id,
                                               const device_status_protocol_upload_t *in_status,
                                               int                                    timeout_ms)
{
    ESP_RETURN_ON_FALSE(in_base_url != NULL && in_base_url[0] != '\0' && timeout_ms > 0
                            && device_status_protocol_values_are_valid(in_status),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "设备状态上传参数无效");

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "创建设备状态 JSON 失败");

    cJSON *environment =
        in_status->has_environment ? cJSON_AddObjectToObject(root, "environment") : NULL;
    cJSON *battery = cJSON_AddObjectToObject(root, "battery");
    bool   complete =
        battery != NULL
        && cJSON_AddNumberToObject(battery, "percent", in_status->battery_percent) != NULL
        && cJSON_AddNumberToObject(battery, "voltage_mv", in_status->battery_voltage_mv) != NULL;
    if (in_status->has_environment)
    {
        complete =
            complete && environment != NULL
            && cJSON_AddNumberToObject(environment, "temperature_c", in_status->temperature_c)
                   != NULL
            && cJSON_AddNumberToObject(environment, "humidity_percent", in_status->humidity_percent)
                   != NULL;
    }

    char *body = complete ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, TAG, "序列化设备状态 JSON 失败");

    char      url[256];
    esp_err_t error = protocol_url_build(url, sizeof(url), in_base_url, "/api/v1/device/status");
    if (error != ESP_OK)
    {
        cJSON_free(body);
        ESP_LOGE(TAG, "构造设备状态 URL 失败: %s", esp_err_to_name(error));
        return error;
    }

    transport_http_header_t headers[4];
    size_t                  header_count = 0U;
    char                    bearer[128]  = { 0 };
    protocol_identity_add_headers(headers,
                                  &header_count,
                                  in_token,
                                  in_device_id,
                                  bearer,
                                  sizeof(bearer));
    headers[header_count++] = (transport_http_header_t){
        .name  = "Content-Type",
        .value = "application/json",
    };
    headers[header_count++] = (transport_http_header_t){
        .name  = "Accept",
        .value = "application/json",
    };
    const transport_http_request_t request = {
        .url                = url,
        .method             = TRANSPORT_HTTP_PUT,
        .headers            = headers,
        .header_count       = header_count,
        .body               = body,
        .body_len           = strlen(body),
        .timeout_ms         = timeout_ms,
        .max_response_bytes = 128U,
    };
    transport_http_response_t response = { 0 };
    error                              = transport_http_perform_borrow(&request, &response);
    cJSON_free(body);
    if (error == ESP_OK && (response.status_code < 200 || response.status_code >= 300))
    {
        ESP_LOGW(TAG, "设备状态上传 HTTP 状态异常: %d", response.status_code);
        error = ESP_ERR_INVALID_RESPONSE;
    }
    transport_http_response_release(&response);
    return error;
}
