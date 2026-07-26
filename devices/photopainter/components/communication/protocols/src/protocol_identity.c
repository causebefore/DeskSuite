/**
 * @file protocol_identity.c
 * @brief 实现业务协议共用的设备身份请求头构造
 */
#include "protocol_identity.h"

#include <stdio.h>

void protocol_identity_add_headers(transport_http_header_t *headers, size_t *count,
                                   const char *token, const char *device_id, char *bearer,
                                   size_t bearer_capacity)
{
    if (device_id != NULL && device_id[0] != '\0')
    {
        headers[(*count)++] = (transport_http_header_t){
            .name  = "X-Device-Id",
            .value = device_id,
        };
    }
    if (token != NULL && token[0] != '\0')
    {
        snprintf(bearer, bearer_capacity, "Bearer %s", token);
        headers[(*count)++] = (transport_http_header_t){
            .name  = "Authorization",
            .value = bearer,
        };
    }
}
