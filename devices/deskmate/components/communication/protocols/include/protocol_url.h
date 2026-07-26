/**
 * @file protocol_url.h
 * @brief 设备服务 URL 规范化与拼接接口
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
 * @brief 规范化服务基础地址并拼接 API 路径
 *
 * 未提供协议时默认使用 http；基础地址末尾与路径开头的斜杠会统一处理。
 *
 * @param[out] out 完整 URL 输出缓冲区
 * @param[in] out_len 输出缓冲区容量
 * @param[in] base_url 服务基础地址
 * @param[in] path API 相对路径
 * @return ESP_OK 构造成功；ESP_ERR_INVALID_ARG 参数或协议无效；
 *         ESP_ERR_INVALID_SIZE 输出缓冲区不足
 */
    esp_err_t protocol_url_build(char *out, size_t out_len, const char *base_url, const char *path);

#ifdef __cplusplus
}
#endif
