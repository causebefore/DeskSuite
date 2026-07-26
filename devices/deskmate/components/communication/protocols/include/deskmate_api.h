/**
 * @file deskmate_api.h
 * @brief 定义 DeskMate Dashboard HTTP 协议
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DESKMATE_API_DEVICE_ID_MAX       32
#define DESKMATE_API_ISO_TIME_MAX        32
#define DESKMATE_API_DASHBOARD_SCHEMA    3
#define DESKMATE_API_DASHBOARD_MAX_BYTES 12288U

    /** @brief DeskMate API 同步请求配置 */
    typedef struct
    {
        const char *base_url;     /*!< 调用期间借用的服务基础地址 */
        const char *device_token; /*!< 调用期间借用的设备令牌 */
        const char *device_id;    /*!< 调用期间借用的稳定设备 ID */
        int         timeout_ms;   /*!< 单次 HTTP 请求超时 */
    } deskmate_api_client_t;

    /** @brief Dashboard 响应及其原始 JSON 所有权 */
    typedef struct
    {
        bool    valid;                                   /*!< 顶层契约是否通过校验 */
        int     schema;                                  /*!< Dashboard schema 版本 */
        char    device_id[DESKMATE_API_DEVICE_ID_MAX];   /*!< 服务端回显的设备 ID */
        char    generated_at[DESKMATE_API_ISO_TIME_MAX]; /*!< 服务端生成时间 */
        int64_t next_refresh_at_utc;                     /*!< 下一次联网刷新 UTC 秒数 */
        char   *raw_json;                                /*!< 成功时由调用方释放 */
        size_t  raw_json_len;                            /*!< 原始 JSON 字节数 */
    } deskmate_api_dashboard_t;

    /**
     * @brief 拉取并校验 DeskMate Dashboard schema 3
     *
     * @param[in] client 请求配置
     * @param[in] max_response_bytes 调用方响应体上限；高于协议上限时自动收紧为
     *                               DESKMATE_API_DASHBOARD_MAX_BYTES
     * @param[out] out Dashboard 输出，成功后必须调用 deskmate_api_dashboard_release()
     * @param[out] http_status 可选 HTTP 状态码输出
     * @return ESP_OK 成功；其他值表示参数、鉴权、传输或响应错误
     */
    esp_err_t deskmate_api_get_dashboard(const deskmate_api_client_t *client, size_t max_response_bytes,
                                         deskmate_api_dashboard_t *out, int *http_status);

    /**
     * @brief 释放 Dashboard 原始 JSON 并清空结构
     *
     * @param[in,out] dashboard 待释放的 Dashboard
     */
    void deskmate_api_dashboard_release(deskmate_api_dashboard_t *dashboard);

#ifdef __cplusplus
}
#endif
