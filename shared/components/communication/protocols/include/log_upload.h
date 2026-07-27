/**
 * @file log_upload.h
 * @brief 封装远端日志会话创建与批量上传协议
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol_backend_context.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 服务端日志会话 ID 缓冲区容量（含结尾空字符） */
#define LOG_UPLOAD_SESSION_ID_MAX 96

    /** @brief 单条待上传日志 */
    typedef struct
    {
        uint32_t seq;          /**< 设备启动期内的日志序号 */
        uint32_t uptime_ms;    /**< 产生日志时的单调运行时间，单位毫秒 */
        char     level[2];     /**< 以空字符结尾的单字符日志级别 */
        char     tag[24];      /**< 以空字符结尾的日志标签 */
        char     message[160]; /**< 以空字符结尾的日志正文 */
        char     raw[224];     /**< 以空字符结尾的原始日志行 */
    } log_upload_line_t;

    /** @brief 创建设备启动日志会话所需的元数据 */
    typedef struct
    {
        const char *firmware_version; /**< 非空固件版本 */
        const char *reset_reason;     /**< 非空复位原因 */
        const char *ip;               /**< 可为空的当前 IPv4 文本 */
    } log_upload_boot_t;

    /**
 * @brief 同步创建一次服务端日志会话
 *
 * 函数仅在调用期间借用所有输入指针。调用方必须提供至少 1 字节的 session_id
 * 缓冲区；参数校验通过后函数会先清空该缓冲区，仅返回 ESP_OK 时其中的会话 ID
 * 有效。
 *
 * @param[in] backend 后端连接、鉴权与设备身份上下文
 * @param[in] timeout_ms HTTP 请求超时，单位毫秒
 * @param[in] boot 设备启动元数据
 * @param[out] session_id 服务端会话 ID 输出缓冲区
 * @param[in] session_id_len 会话 ID 输出缓冲区容量，必须大于 0
 * @return ESP_OK 会话已创建；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_RESPONSE 响应缺少会话 ID；或内存、网络及 HTTP 错误码
 */
    esp_err_t log_upload_start(const protocol_backend_context_t *backend, int timeout_ms, const log_upload_boot_t *boot,
                               char *session_id, size_t session_id_len);

    /**
 * @brief 同步上传一批日志并接收服务端后续会话 ID
 *
 * 函数仅在调用期间借用所有输入指针。session_id 可为空；next_session_id 可为
 * NULL。 服务端未返回新会话 ID 时不会改写
 * next_session_id，调用方需要区分该情况时应预先清空。
 *
 * @param[in] backend 后端连接、鉴权与设备身份上下文
 * @param[in] timeout_ms HTTP 请求超时，单位毫秒
 * @param[in] session_id 当前会话 ID；NULL 或空字符串表示尚无会话
 * @param[in] lines 待上传日志数组
 * @param[in] count 日志条数
 * @param[out] next_session_id 可选的后续会话 ID 输出缓冲区
 * @param[in] next_session_id_len 后续会话 ID 输出缓冲区容量
 * @return ESP_OK 批次已被服务端接受；ESP_ERR_INVALID_ARG 参数无效；
 *         或内存、网络及 HTTP 错误码
 */
    esp_err_t log_upload_batch(const protocol_backend_context_t *backend, int timeout_ms, const char *session_id,
                               const log_upload_line_t *lines, size_t count, char *next_session_id,
                               size_t next_session_id_len);

#ifdef __cplusplus
}
#endif
