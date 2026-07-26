/*
 * 文件职责：提供不感知业务协议的同步 HTTP 缓冲式请求能力。
 * 主要依赖：ESP-IDF esp_http_client。
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

    typedef enum
    {
        TRANSPORT_HTTP_GET = 0,
        TRANSPORT_HTTP_POST,
        TRANSPORT_HTTP_PUT,
        TRANSPORT_HTTP_PATCH,
        TRANSPORT_HTTP_DELETE,
    } transport_http_method_t;

    typedef struct
    {
        const char *name;
        const char *value;
    } transport_http_header_t;

    typedef struct
    {
        const char                    *url;
        transport_http_method_t        method;
        const transport_http_header_t *headers;
        size_t                         header_count;
        const void                    *body;
        size_t                         body_len;
        int                            timeout_ms;
        size_t                         max_response_bytes;
        bool                           suppress_success_log; /**< 成功时不输出逐请求耗时日志 */
    } transport_http_request_t;

    typedef struct
    {
        int    status_code;
        char  *body;
        size_t body_len;
    } transport_http_response_t;

    /**
 * @brief 借用请求描述并执行同步 HTTP 缓冲式请求
 *
 * request 及其 URL、请求头和请求体只在调用期间借用，函数返回后不再持有这些指针。
 * 成功时 response->body 由调用方持有，使用后必须调用 transport_http_response_release()。
 * suppress_success_log 只抑制成功请求的耗时日志，失败诊断仍会正常输出。
 *
 * @param[in] request 调用期间借用的请求描述
 * @param[out] response 响应输出
 * @return ESP_OK 请求完成；其他值表示参数、内存或网络错误
 */
    esp_err_t transport_http_perform_borrow(const transport_http_request_t *request,
                                            transport_http_response_t      *response);
    void      transport_http_response_release(transport_http_response_t *response);

    typedef esp_err_t (*transport_http_data_cb_t)(const uint8_t *data, size_t len, void *ctx);
    typedef bool (*transport_http_continue_cb_t)(void *ctx);

    typedef struct
    {
        const char                    *url;
        const transport_http_header_t *headers;
        size_t                         header_count;
        const uint8_t                 *upload_data;
        size_t                         upload_len;
        size_t                         read_buffer_bytes;
        int                            timeout_ms;
        transport_http_data_cb_t       on_response_data;
        transport_http_continue_cb_t   should_continue;
        void                          *ctx;
    } transport_http_stream_request_t;

    typedef struct
    {
        int    status_code;
        size_t uploaded_bytes;
        size_t received_bytes;
    } transport_http_stream_result_t;

    /**
 * @brief 借用请求描述并执行同步 HTTP 流式上传和响应读取
 *
 * request 内的上传数据、请求头、回调和上下文只在调用期间借用，函数返回后不再持有。
 *
 * @param[in] request 调用期间借用的流式请求描述
 * @param[out] result 流式传输结果
 * @return ESP_OK 传输完成；其他值表示取消、参数、内存或网络错误
 */
    esp_err_t transport_http_stream_borrow(const transport_http_stream_request_t *request,
                                           transport_http_stream_result_t        *result);

    /** @brief HTTP 下载响应信息 */
    typedef struct
    {
        int     status_code;    /**< HTTP 状态码 */
        int64_t content_length; /**< Content-Length，未知时为负数 */
        size_t  received_bytes; /**< 已接收响应体字节数 */
    } transport_http_download_result_t;

    /** @brief HTTP GET 流式下载请求 */
    typedef struct
    {
        const char                    *url;               /**< 下载地址 */
        const transport_http_header_t *headers;           /**< 请求头数组 */
        size_t                         header_count;      /**< 请求头数量 */
        size_t                         read_buffer_bytes; /**< 接收缓冲区大小 */
        int                            timeout_ms;        /**< 请求超时 */
        transport_http_data_cb_t       on_response_data;  /**< 响应数据回调 */
        transport_http_continue_cb_t   should_continue;   /**< 可选取消回调 */
        void                          *ctx;               /**< 用户上下文 */
    } transport_http_download_request_t;

    /**
 * @brief 使用 GET 流式下载响应体
 *
 * 响应数据直接交给回调，不在传输层缓存完整文件。request 内的 URL、请求头、回调和
 * 上下文只在调用期间借用，函数返回后不再持有。
 *
 * @param[in] request 调用期间借用的下载请求
 * @param[out] result 下载结果
 * @return ESP_OK 下载完成；ESP_ERR_INVALID_RESPONSE 表示非 2xx；或其他网络错误码
 */
    esp_err_t transport_http_download_borrow(const transport_http_download_request_t *request,
                                             transport_http_download_result_t        *result);

#ifdef __cplusplus
}
#endif
