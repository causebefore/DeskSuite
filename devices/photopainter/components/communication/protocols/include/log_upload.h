/* 文件职责：封装远端日志启动与批量上传协议。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LOG_UPLOAD_SESSION_ID_MAX 96

    /** @brief 单条远端日志的结构化字段 */
    typedef struct
    {
        uint32_t seq;
        uint32_t uptime_ms;
        char     level[2];
        char     tag[24];
        char     message[160];
        char     raw[224];
    } log_upload_line_t;

    /** @brief 创建远端日志会话所需的启动信息 */
    typedef struct
    {
        uint32_t    product_id; /**< 产品标识，必须大于 0 */
        const char *device_id;
        const char *firmware_version;
        const char *reset_reason;
        const char *ip;
    } log_upload_boot_t;

    /**
     * @brief 创建服务端日志会话
     *
     * @param[in] base_url 服务端基础 URL
     * @param[in] timeout_ms HTTP 超时，单位毫秒
     * @param[in] boot 含产品、设备和启动信息的请求体，仅在调用期间借用
     * @param[out] session_id 服务端返回的会话 ID 输出
     * @param[in] session_id_len 会话 ID 输出缓冲区容量
     * @return ESP_OK 成功；其他值为参数、传输或响应错误
     */
    esp_err_t log_upload_start(const char *base_url, int timeout_ms, const log_upload_boot_t *boot,
                               char *session_id, size_t session_id_len);

    /**
     * @brief 向指定产品和设备的会话批量上传日志
     *
     * @param[in] base_url 服务端基础 URL
     * @param[in] timeout_ms HTTP 超时，单位毫秒
     * @param[in] product_id 产品标识，必须大于 0
     * @param[in] device_id 设备标识
     * @param[in] session_id 当前会话 ID，可为空
     * @param[in] lines 待上传日志数组，仅在调用期间借用
     * @param[in] count 日志条数
     * @param[out] next_session_id 服务端返回的会话 ID 输出，可为空
     * @param[in] next_session_id_len 会话 ID 输出缓冲区容量
     * @return ESP_OK 成功；其他值为参数、传输或响应错误
     */
    esp_err_t log_upload_batch(const char *base_url, int timeout_ms, uint32_t product_id,
                               const char *device_id, const char *session_id,
                               const log_upload_line_t *lines, size_t count, char *next_session_id,
                               size_t next_session_id_len);

#ifdef __cplusplus
}
#endif
