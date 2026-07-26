/**
 * @file protocol_identity.h
 * @brief 声明业务协议共用的设备身份请求头构造接口
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "transport.h"

#define PROTOCOL_IDENTITY_DEVICE_ID_MAX 32

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
void protocol_identity_add_headers(transport_http_header_t *headers, size_t *count, const char *token,
                                   const char *device_id, char *bearer, size_t bearer_capacity);

/**
 * @brief 格式化 WebSocket 握手使用的设备身份请求头
 *
 * 输出采用 ESP WebSocket 客户端要求的原始 HTTP 头格式。设备 ID 非空时写入
 * `X-Device-Id`，令牌非空时写入 `Authorization: Bearer ...`。
 *
 * @param[out] headers 输出缓冲区
 * @param[in] headers_capacity 输出缓冲区容量
 * @param[in] token 可为空的设备令牌
 * @param[in] device_id 可为空的设备 ID
 * @return ESP_OK 格式化成功；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_SIZE 输出缓冲区不足
 */
esp_err_t protocol_identity_format_websocket_headers(char *headers, size_t headers_capacity, const char *token,
                                                     const char *device_id);
