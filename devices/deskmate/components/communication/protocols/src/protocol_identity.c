/**
 * @file protocol_identity.c
 * @brief 实现业务协议共用的设备身份请求头构造
 */
#include "protocol_identity.h"

#include <stdio.h>
#include <string.h>

void protocol_identity_add_headers(transport_http_header_t *headers, size_t *count, const char *token,
                                   const char *device_id, char *bearer, size_t bearer_capacity)
{
    if (device_id != NULL && device_id[0] != '\0')
    {
        headers[(*count)++] = (transport_http_header_t) {
            .name  = "X-Device-Id",
            .value = device_id,
        };
    }
    if (token != NULL && token[0] != '\0')
    {
        snprintf(bearer, bearer_capacity, "Bearer %s", token);
        headers[(*count)++] = (transport_http_header_t) {
            .name  = "Authorization",
            .value = bearer,
        };
    }
}

/** @brief 向 WebSocket 原始头缓冲区追加一行并校验容量 */
static esp_err_t append_websocket_header(char *headers, size_t headers_capacity, const char *format, const char *value)
{
    const size_t used = strlen(headers);
    if (used >= headers_capacity)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const int written = snprintf(headers + used, headers_capacity - used, format, value);
    return written >= 0 && (size_t) written < headers_capacity - used ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t protocol_identity_format_websocket_headers(char *headers, size_t headers_capacity, const char *token,
                                                     const char *device_id)
{
    if (headers == NULL || headers_capacity == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    headers[0] = '\0';
    if (device_id != NULL && device_id[0] != '\0')
    {
        const esp_err_t error = append_websocket_header(headers, headers_capacity, "X-Device-Id: %s\r\n", device_id);
        if (error != ESP_OK)
        {
            return error;
        }
    }
    if (token != NULL && token[0] != '\0')
    {
        return append_websocket_header(headers, headers_capacity, "Authorization: Bearer %s\r\n", token);
    }
    return ESP_OK;
}
