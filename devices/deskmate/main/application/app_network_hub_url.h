/**
 * @file app_network_hub_url.h
 * @brief DeskMate Hub 地址的纯解析与规范化接口
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Hub 地址规范化结果的最大 ASCII 字节数，不含结尾 NUL。 */
#define APP_NETWORK_HUB_URL_MAX_LENGTH 127U

    /**
     * @brief 解析并复制一个仅含 HTTP authority 的 Hub 地址
     *
     * 接受 `http://`、IPv4 或 hostname 以及可选端口；scheme 与 host 会转成小写，唯一的
     * 末尾斜杠会被移除。输出不包含业务 path、query、fragment 或尾部斜杠。失败时若输出
     * 缓冲区有效，会把首字节清零。
     *
     * @param[in] input 待解析的 NUL 结尾 ASCII 地址
     * @param[out] out_url 调用方提供的 128 字节规范化地址输出
     * @return ESP_OK 地址有效；ESP_ERR_INVALID_ARG 参数或 URL 语法无效；
     *         ESP_ERR_INVALID_SIZE 规范化结果超过 127 字节
     */
    esp_err_t app_network_hub_url_parse_copy(
        const char *input,
        char out_url[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U]);

#ifdef __cplusplus
}
#endif
