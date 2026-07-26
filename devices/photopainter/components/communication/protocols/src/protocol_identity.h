/**
 * @file protocol_identity.h
 * @brief 业务协议共用的设备身份请求头构造
 */
#pragma once

#include <stddef.h>

#include "transport.h"

/**
 * @brief 向请求头数组追加可选的设备 ID 与 Bearer Token
 *
 * 调用方必须提供至少两个可写槽位；bearer 必须足以保存 `Bearer ` 前缀与 token。
 *
 * @param[out] headers 请求头数组
 * @param[in,out] count 当前请求头数量
 * @param[in] token 可为空的设备令牌
 * @param[in] device_id 可为空的设备 ID
 * @param[out] bearer Authorization 值缓冲区
 * @param[in] bearer_capacity bearer 缓冲区容量
 */
void protocol_identity_add_headers(transport_http_header_t *headers, size_t *count,
                                   const char *token, const char *device_id, char *bearer,
                                   size_t bearer_capacity);
