/**
 * @file transport_http.h
 * @brief 提供不感知业务协议的同步 HTTP 缓冲与流式传输能力
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

    /** @brief HTTP 请求方法 */
    typedef enum
    {
        TRANSPORT_HTTP_GET = 0, /**< GET */
        TRANSPORT_HTTP_POST,    /**< POST */
        TRANSPORT_HTTP_PUT,     /**< PUT */
        TRANSPORT_HTTP_PATCH,   /**< PATCH */
        TRANSPORT_HTTP_DELETE,  /**< DELETE */
    } transport_http_method_t;

    /** @brief 单个 HTTP 请求头 */
    typedef struct
    {
        const char *name;  /**< 调用期间借用的请求头名称 */
        const char *value; /**< 调用期间借用的请求头值 */
    } transport_http_header_t;

    /** @brief 同步缓冲式 HTTP 请求描述 */
    typedef struct
    {
        const char                    *url;                  /**< 请求 URL */
        transport_http_method_t        method;               /**< 请求方法 */
        const transport_http_header_t *headers;              /**< header_count 为 0 时可为空的请求头数组 */
        size_t                         header_count;         /**< 请求头数量 */
        const void                    *body;                 /**< 可为空的请求体 */
        size_t                         body_len;             /**< 请求体字节数 */
        int                            timeout_ms;           /**< HTTP 请求超时，单位毫秒 */
        size_t                         max_response_bytes;   /**< 最大响应体字节数；0 使用默认上限 */
        bool                           suppress_success_log; /**< 成功时不输出逐请求耗时日志 */
    } transport_http_request_t;

    /** @brief 缓冲式 HTTP 响应及其动态内存所有权 */
    typedef struct
    {
        int    status_code; /**< HTTP 状态码 */
        char  *body;        /**< 成功时由调用方持有的零结尾响应体 */
        size_t body_len;    /**< 不含结尾空字符的响应体字节数 */
    } transport_http_response_t;

    /**
 * @brief 借用请求描述并执行同步 HTTP 缓冲式请求
 *
 * request 及其 URL、请求头和请求体只在调用期间借用，函数返回后不再持有这些指针。
 * ESP_OK 只表示传输完成，不保证 HTTP 状态为 2xx。成功时 response->body 由调用方持有，
 * 使用后必须调用 transport_http_response_release()。
 * suppress_success_log 只抑制成功请求的耗时日志，失败诊断仍会正常输出。
 *
 * @param[in] request 调用期间借用的请求描述
 * @param[out] response 响应输出
 * @return ESP_OK 请求完成；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NO_MEM 响应超过上限或
 *         内存不足；其他值表示网络错误
 */
    esp_err_t transport_http_perform_borrow(const transport_http_request_t *request,
                                            transport_http_response_t      *response);

    /**
     * @brief 释放缓冲式 HTTP 响应体并清空结构
     *
     * response 为 NULL 或结构已经清空时保持幂等。
     *
     * @param[in,out] response 待释放响应
     */
    void transport_http_response_release(transport_http_response_t *response);

    /**
     * @brief 同步处理一段 HTTP 响应数据
     *
     * 回调在发起传输的调用上下文执行，data 仅在本次回调期间有效。返回错误会立即停止传输，
     * 并由对应传输 API 原样返回。
     *
     * @param[in] data 响应数据分片
     * @param[in] len 数据分片字节数
     * @param[in] ctx 调用方上下文
     * @return ESP_OK 已消费本分片；其他值表示停止传输的具体错误
     */
    typedef esp_err_t (*transport_http_data_cb_t)(const uint8_t *data, size_t len, void *ctx);

    /**
     * @brief 查询同步 HTTP 传输是否继续
     *
     * 回调在发起传输的调用上下文、两次 I/O 操作之间执行，必须快速返回。
     *
     * @param[in] ctx 调用方上下文
     * @return true 继续传输；false 协作取消传输
     */
    typedef bool (*transport_http_continue_cb_t)(void *ctx);

    /** @brief 同步 HTTP 流式上传与响应读取请求 */
    typedef struct
    {
        const char                    *url;               /**< 请求 URL */
        const transport_http_header_t *headers;           /**< header_count 为 0 时可为空的请求头数组 */
        size_t                         header_count;      /**< 请求头数量 */
        const uint8_t                 *upload_data;       /**< 待上传数据 */
        size_t                         upload_len;        /**< 待上传字节数 */
        size_t                         read_buffer_bytes; /**< 响应读取缓冲区；0 使用默认值 */
        int                            timeout_ms;        /**< HTTP 请求超时，单位毫秒 */
        transport_http_data_cb_t       on_response_data;  /**< 响应数据同步回调 */
        transport_http_continue_cb_t   should_continue;   /**< 可为空的协作取消回调 */
        void                          *ctx;               /**< 回调上下文 */
    } transport_http_stream_request_t;

    /** @brief HTTP 流式上传与响应读取进度结果 */
    typedef struct
    {
        int    status_code;    /**< HTTP 状态码；尚未取得时为 0 */
        size_t uploaded_bytes; /**< 已上传字节数 */
        size_t received_bytes; /**< 已接收字节数 */
    } transport_http_stream_result_t;

    /**
 * @brief 借用请求描述并执行同步 HTTP 流式上传和响应读取
 *
 * request 内的上传数据、请求头、回调和上下文只在调用期间借用，函数返回后不再持有。
 * 参数校验通过后 result 会先清零；错误返回时仍可读取已完成的传输计数。
 *
 * @param[in] request 调用期间借用的流式请求描述
 * @param[out] result 流式传输结果
 * @return ESP_OK 传输完成；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 调用方取消；
 *         ESP_FAIL HTTP 状态非 2xx 或底层 I/O 失败；或其他内存、网络错误码
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
        const transport_http_header_t *headers;           /**< header_count 为 0 时可为空的请求头数组 */
        size_t                         header_count;      /**< 请求头数量 */
        size_t                         read_buffer_bytes; /**< 接收缓冲区大小 */
        int                            timeout_ms;        /**< 请求超时 */
        bool                           automatic_redirects; /**< 是否自动跟随 HTTP 重定向 */
        transport_http_data_cb_t       on_response_data;  /**< 响应数据回调 */
        transport_http_continue_cb_t   should_continue;   /**< 可选取消回调 */
        void                          *ctx;               /**< 用户上下文 */
    } transport_http_download_request_t;

    /**
 * @brief 使用 GET 流式下载响应体
 *
 * 响应数据直接交给回调，不在传输层缓存完整文件。request 内的 URL、请求头、回调和
 * 上下文只在调用期间借用，函数返回后不再持有。automatic_redirects 开启时最多跟随五次
 * HTTP 重定向，且请求头会沿用到重定向目标；调用方必须确保其中不含跨域敏感凭据。
 * 参数校验通过后 result 会先初始化；错误返回时仍可读取状态码、Content-Length 和已接收
 * 字节数。
 *
 * @param[in] request 调用期间借用的下载请求
 * @param[out] result 下载结果
 * @return ESP_OK 下载完成；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 调用方取消；
 *         ESP_ERR_INVALID_RESPONSE HTTP 状态非 2xx；ESP_ERR_INVALID_SIZE 响应长度不一致；
 *         或其他内存、网络错误码
 */
    esp_err_t transport_http_download_borrow(const transport_http_download_request_t *request,
                                             transport_http_download_result_t        *result);

#ifdef __cplusplus
}
#endif
