/**
 * @file protocol_identity.h
 * @brief 声明业务协议共用的设备身份请求头构造接口
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "transport.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PROTOCOL_IDENTITY_DEVICE_ID_MAX 32

    /**
 * @brief 基于 Wi-Fi Station 基础 MAC 生成稳定硬件设备 ID
 *
 * 输出格式固定为
 * `esp32-xxxxxxxxxxxx`，不会读取产品配置或持久化存储。发生错误时输出缓冲区
 * 保持为空，不生成可能发生设备碰撞的占位 ID。
 *
 * @param[out] out_device_id 设备 ID 输出缓冲区
 * @param[in] capacity 输出缓冲区容量
 * @return ESP_OK 已生成；ESP_ERR_INVALID_ARG 参数或容量不足；或读取基础 MAC
 * 的错误码
 */
    esp_err_t protocol_identity_get_hardware_device_id_copy(char *out_device_id, size_t capacity);

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

#ifdef __cplusplus
}
#endif
