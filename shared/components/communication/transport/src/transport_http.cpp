/**
 * @file transport_http.cpp
 * @brief 使用 RAII 管理 HTTP Client 与临时缓冲区的同步传输实现
 */
#include "transport_http.h"

#include <cstdlib>
#include <cstring>
#include <memory>

#include "sdkconfig.h"
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#define DEFAULT_RESPONSE_LIMIT (16U * 1024U)
#define TRANSPORT_HTTP_MAX_REDIRECTS 5U

/** @brief 日志标签 */
static const char *TAG = "transport_http";

/** @brief 使用 free() 释放 C 堆内存的 unique_ptr 删除器 */
struct TransportHttpFreeDeleter
{
    /**
     * @brief 释放缓冲区
     *
     * @param[in] pointer 待释放指针
     */
    void operator()(void *pointer) const noexcept
    {
        free(pointer);
    }
};

template <typename T> using TransportHttpHeapPtr = std::unique_ptr<T, TransportHttpFreeDeleter>;

/** @brief HTTP 响应缓冲上下文 */
typedef struct
{
    char   *data;
    size_t  len;
    size_t  capacity;
    bool    overflow;
    int64_t started_us;
    int64_t connected_us;
    int64_t headers_sent_us;
    int64_t first_response_us;
    int64_t finished_us;
} response_buffer_t;

/** @brief 自动关闭并销毁 esp_http_client 句柄 */
class TransportHttpClient final {
  public:
    /**
     * @brief 创建 HTTP Client
     *
     * @param[in] config ESP-IDF HTTP Client 配置
     */
    explicit TransportHttpClient(const esp_http_client_config_t &config) noexcept
        : handle_(esp_http_client_init(&config))
    {
    }

    /** @brief 关闭已打开连接并销毁 HTTP Client */
    ~TransportHttpClient()
    {
        if (handle_ == nullptr)
        {
            return;
        }
        if (opened_)
        {
            (void) esp_http_client_close(handle_);
        }
        esp_http_client_cleanup(handle_);
    }

    /** @brief 禁止复制 HTTP Client 所有权 */
    TransportHttpClient(const TransportHttpClient &)            = delete;

    /** @brief 禁止复制 HTTP Client 所有权 */
    TransportHttpClient &operator=(const TransportHttpClient &) = delete;

    /**
     * @brief 取得底层句柄
     *
     * @return HTTP Client 句柄；创建失败时为 nullptr
     */
    esp_http_client_handle_t get() const noexcept
    {
        return handle_;
    }

    /**
     * @brief 打开 HTTP 连接并记录关闭责任
     *
     * @param[in] write_len 请求体长度
     * @return ESP_OK 已打开；其他值为 ESP-IDF 错误码
     */
    esp_err_t open(int write_len) noexcept
    {
        if (handle_ == nullptr)
        {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t err = esp_http_client_open(handle_, write_len);
        opened_             = err == ESP_OK;
        return err;
    }

    /**
     * @brief 关闭当前 HTTP 连接并保留 Client 配置
     *
     * @return ESP_OK 已关闭或原本未打开；其他值为 ESP-IDF 错误码
     */
    esp_err_t close() noexcept
    {
        if (handle_ == nullptr)
        {
            return ESP_ERR_INVALID_STATE;
        }
        if (!opened_)
        {
            return ESP_OK;
        }
        const esp_err_t err = esp_http_client_close(handle_);
        if (err == ESP_OK)
        {
            opened_ = false;
        }
        return err;
    }

  private:
    esp_http_client_handle_t handle_ = nullptr; /**< 底层 HTTP Client 句柄 */
    bool                     opened_ = false;   /**< 是否需要显式关闭连接 */
};

/** @brief 判断状态码是否要求客户端使用 Location 发起下一次请求 */
static bool transport_http_status_is_redirect(int status_code)
{
    return status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307
           || status_code == 308;
}

/**
 * @brief 为 HTTPS Client 挂载 ESP-IDF 系统证书包
 *
 * @param[in,out] config 待补充的 Client 配置
 * @param[in] url 请求 URL
 * @return ESP_OK 已配置或当前为 HTTP；ESP_ERR_NOT_SUPPORTED 固件未启用证书包
 */
static esp_err_t transport_http_configure_tls(esp_http_client_config_t *config, const char *url)
{
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config->crt_bundle_attach = esp_crt_bundle_attach;
    (void) url;
    return ESP_OK;
#else
    if (strncmp(url, "https://", sizeof("https://") - 1U) == 0)
    {
        ESP_LOGE(TAG, "HTTPS 请求需要启用系统证书包");
        return ESP_ERR_NOT_SUPPORTED;
    }
    (void) config;
    return ESP_OK;
#endif
}

/**
 * @brief 打开下载响应，并按请求契约显式跟随有限次重定向
 *
 * 原生 open/fetch_headers/read 流程不会自动推进重定向，因此在读取任何响应体前解析
 * Location、关闭当前连接并重新打开。调用方负责保证允许重定向时的请求头可安全跨域沿用。
 *
 * @param[in,out] client 已配置请求 URL 与请求头的 HTTP Client
 * @param[in] automatic_redirects 是否跟随重定向
 * @param[out] result 最终响应状态与长度
 * @return ESP_OK 已取得 2xx 最终响应；其他值表示响应、重定向或传输失败
 */
static esp_err_t transport_http_open_download_response(TransportHttpClient              *client,
                                                       bool                              automatic_redirects,
                                                       transport_http_download_result_t *result)
{
    size_t redirect_count = 0U;
    while (true)
    {
        esp_err_t error = client->open(0);
        if (error != ESP_OK)
        {
            return error;
        }
        const int64_t content_length = esp_http_client_fetch_headers(client->get());
        if (content_length < 0)
        {
            return ESP_ERR_HTTP_FETCH_HEADER;
        }
        result->status_code = esp_http_client_get_status_code(client->get());
        result->content_length =
            esp_http_client_is_chunked_response(client->get()) ? -1 : content_length;
        if (!transport_http_status_is_redirect(result->status_code))
        {
            return result->status_code >= 200 && result->status_code < 300 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        if (!automatic_redirects)
        {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (redirect_count >= TRANSPORT_HTTP_MAX_REDIRECTS)
        {
            return ESP_ERR_HTTP_MAX_REDIRECT;
        }
        error = esp_http_client_set_redirection(client->get());
        if (error == ESP_OK)
        {
            error = esp_http_client_clear_response_buffer(client->get());
        }
        if (error == ESP_OK)
        {
            error = client->close();
        }
        if (error != ESP_OK)
        {
            return error;
        }
        ++redirect_count;
    }
}

/**
 * @brief 将项目 HTTP 方法转换为 ESP-IDF 方法
 *
 * @param[in] method 项目 HTTP 方法
 * @return ESP-IDF HTTP 方法
 */
static esp_http_client_method_t to_idf_method(transport_http_method_t method)
{
    switch (method)
    {
        case TRANSPORT_HTTP_POST:
            return HTTP_METHOD_POST;
        case TRANSPORT_HTTP_PUT:
            return HTTP_METHOD_PUT;
        case TRANSPORT_HTTP_PATCH:
            return HTTP_METHOD_PATCH;
        case TRANSPORT_HTTP_DELETE:
            return HTTP_METHOD_DELETE;
        case TRANSPORT_HTTP_GET:
        default:
            return HTTP_METHOD_GET;
    }
}

/**
 * @brief 收集缓冲式请求的响应数据
 *
 * @param[in] event HTTP Client 事件
 * @return ESP_OK 已接收；ESP_FAIL 响应超过容量
 */
static esp_err_t on_http_event(esp_http_client_event_t *event)
{
    if (event == nullptr || event->user_data == nullptr)
    {
        return ESP_OK;
    }

    auto         *buffer = static_cast<response_buffer_t *>(event->user_data);
    const int64_t now_us = esp_timer_get_time();
    switch (event->event_id)
    {
        case HTTP_EVENT_ON_CONNECTED:
            if (buffer->connected_us == 0)
            {
                buffer->connected_us = now_us;
            }
            break;
        case HTTP_EVENT_HEADERS_SENT:
            if (buffer->headers_sent_us == 0)
            {
                buffer->headers_sent_us = now_us;
            }
            break;
        case HTTP_EVENT_ON_HEADER:
        case HTTP_EVENT_ON_HEADERS_COMPLETE:
        case HTTP_EVENT_ON_DATA:
            if (buffer->first_response_us == 0)
            {
                buffer->first_response_us = now_us;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            buffer->finished_us = now_us;
            break;
        default:
            break;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr || event->data_len <= 0)
    {
        return ESP_OK;
    }

    const size_t chunk_len = static_cast<size_t>(event->data_len);
    if (buffer->len + chunk_len >= buffer->capacity)
    {
        buffer->overflow = true;
        return ESP_FAIL;
    }

    memcpy(buffer->data + buffer->len, event->data, chunk_len);
    buffer->len += chunk_len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

/**
 * @brief 计算 HTTP 阶段耗时，缺失事件时返回 -1
 *
 * @param[in] started_us 阶段起点
 * @param[in] finished_us 阶段终点
 * @return 阶段耗时，单位 ms；事件缺失或顺序异常时为 -1
 */
#if CONFIG_COMMUNICATION_HTTP_TIMING_LOGS
static long long transport_http_elapsed_ms(int64_t started_us, int64_t finished_us)
{
    return started_us > 0 && finished_us >= started_us ? static_cast<long long>((finished_us - started_us) / 1000LL)
                                                       : -1LL;
}
#endif

/** @brief 输出缓冲式 HTTP 请求的分阶段耗时 */
static void transport_http_log_timing(const char *url, int status, esp_err_t error, const response_buffer_t &buffer,
                                      int64_t returned_us)
{
#if CONFIG_COMMUNICATION_HTTP_TIMING_LOGS
    const int64_t finished_us = buffer.finished_us > 0 ? buffer.finished_us : returned_us;
    ESP_LOGI(TAG,
             "HTTP 请求耗时：url=%s status=%d result=%s total=%lld ms "
             "建连=%lld ms 发送请求=%lld ms 等待首包=%lld ms 接收响应=%lld ms",
             url,
             status,
             esp_err_to_name(error),
             transport_http_elapsed_ms(buffer.started_us, returned_us),
             transport_http_elapsed_ms(buffer.started_us, buffer.connected_us),
             transport_http_elapsed_ms(buffer.connected_us, buffer.headers_sent_us),
             transport_http_elapsed_ms(buffer.headers_sent_us, buffer.first_response_us),
             transport_http_elapsed_ms(buffer.first_response_us, finished_us));
#else
    (void) url;
    (void) status;
    (void) error;
    (void) buffer;
    (void) returned_us;
#endif
}

void transport_http_response_release(transport_http_response_t *response)
{
    if (response == nullptr)
    {
        return;
    }
    free(response->body);
    memset(response, 0, sizeof(*response));
}

esp_err_t transport_http_perform_borrow(const transport_http_request_t *request, transport_http_response_t *response)
{
    if (request == nullptr || request->url == nullptr || request->url[0] == '\0' || response == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(response, 0, sizeof(*response));
    const size_t capacity =
        (request->max_response_bytes > 0 ? request->max_response_bytes : DEFAULT_RESPONSE_LIMIT) + 1;
    TransportHttpHeapPtr<char> response_data(static_cast<char *>(calloc(1, capacity)));
    if (response_data == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    response_buffer_t buffer        = {};
    buffer.data                     = response_data.get();
    buffer.capacity                 = capacity;

    esp_http_client_config_t config = {};
    config.url                      = request->url;
    config.timeout_ms               = request->timeout_ms;
    config.event_handler            = on_http_event;
    config.user_data                = &buffer;
    esp_err_t err                   = transport_http_configure_tls(&config, request->url);
    if (err != ESP_OK)
    {
        return err;
    }
    TransportHttpClient client(config);
    if (client.get() == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_set_method(client.get(), to_idf_method(request->method));
    for (size_t i = 0; err == ESP_OK && i < request->header_count; ++i)
    {
        if (request->headers[i].name == nullptr || request->headers[i].value == nullptr)
        {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = esp_http_client_set_header(client.get(), request->headers[i].name, request->headers[i].value);
    }
    if (err == ESP_OK && request->body != nullptr)
    {
        err = esp_http_client_set_post_field(client.get(),
                                             static_cast<const char *>(request->body),
                                             static_cast<int>(request->body_len));
    }
    if (err == ESP_OK)
    {
        buffer.started_us = esp_timer_get_time();
        err               = esp_http_client_perform(client.get());
    }

    const int64_t returned_us = esp_timer_get_time();
    const int     status      = esp_http_client_get_status_code(client.get());
    if (err == ESP_ERR_NOT_SUPPORTED && status > 0)
    {
        err = ESP_OK;
    }
    if (buffer.overflow)
    {
        err = ESP_ERR_NO_MEM;
    }
    const bool request_succeeded = err == ESP_OK && status >= 200 && status < 300;
    if (buffer.started_us > 0 && (!request->suppress_success_log || !request_succeeded))
    {
        transport_http_log_timing(request->url, status, err, buffer, returned_us);
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "HTTP 请求失败：url=%s err=%s", request->url, esp_err_to_name(err));
        return err;
    }

    response->status_code = status;
    response->body        = response_data.release();
    response->body_len    = buffer.len;
    return ESP_OK;
}

esp_err_t transport_http_stream_borrow(const transport_http_stream_request_t *request,
                                       transport_http_stream_result_t        *result)
{
    if (request == nullptr || request->url == nullptr || request->upload_data == nullptr || request->upload_len == 0
        || request->on_response_data == nullptr || result == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    esp_http_client_config_t config = {};
    config.url                      = request->url;
    config.timeout_ms               = request->timeout_ms;
    config.method                   = HTTP_METHOD_POST;
    config.buffer_size              = static_cast<int>(request->read_buffer_bytes);
    esp_err_t err                   = transport_http_configure_tls(&config, request->url);
    if (err != ESP_OK)
    {
        return err;
    }
    TransportHttpClient client(config);
    if (client.get() == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    err = ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < request->header_count; ++i)
    {
        if (request->headers[i].name == nullptr || request->headers[i].value == nullptr)
        {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = esp_http_client_set_header(client.get(), request->headers[i].name, request->headers[i].value);
    }
    if (err == ESP_OK)
    {
        err = client.open(static_cast<int>(request->upload_len));
    }
    while (err == ESP_OK && result->uploaded_bytes < request->upload_len)
    {
        if (request->should_continue != nullptr && !request->should_continue(request->ctx))
        {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        const int written =
            esp_http_client_write(client.get(),
                                  reinterpret_cast<const char *>(request->upload_data) + result->uploaded_bytes,
                                  static_cast<int>(request->upload_len - result->uploaded_bytes));
        if (written <= 0)
        {
            err = ESP_FAIL;
            break;
        }
        result->uploaded_bytes += static_cast<size_t>(written);
    }
    if (err == ESP_OK)
    {
        (void) esp_http_client_fetch_headers(client.get());
        result->status_code = esp_http_client_get_status_code(client.get());
        if (result->status_code < 200 || result->status_code >= 300)
        {
            err = ESP_FAIL;
        }
    }

    const size_t                  read_size = request->read_buffer_bytes > 0 ? request->read_buffer_bytes : 2048;
    TransportHttpHeapPtr<uint8_t> buffer(
        static_cast<uint8_t *>(heap_caps_malloc(read_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    if (err == ESP_OK && buffer == nullptr)
    {
        err = ESP_ERR_NO_MEM;
    }
    while (err == ESP_OK)
    {
        if (request->should_continue != nullptr && !request->should_continue(request->ctx))
        {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        const int received =
            esp_http_client_read(client.get(), reinterpret_cast<char *>(buffer.get()), static_cast<int>(read_size));
        if (received < 0)
        {
            err = ESP_FAIL;
            break;
        }
        if (received == 0)
        {
            break;
        }
        result->received_bytes += static_cast<size_t>(received);
        err = request->on_response_data(buffer.get(), static_cast<size_t>(received), request->ctx);
    }
    return err;
}

esp_err_t transport_http_download_borrow(const transport_http_download_request_t *request,
                                         transport_http_download_result_t        *result)
{
    if (request == nullptr || request->url == nullptr || request->url[0] == '\0' || request->on_response_data == nullptr
        || result == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->content_length          = -1;
    const size_t read_size          = request->read_buffer_bytes > 0U ? request->read_buffer_bytes : 4096U;

    esp_http_client_config_t config = {};
    config.url                      = request->url;
    config.timeout_ms               = request->timeout_ms;
    config.method                   = HTTP_METHOD_GET;
    config.buffer_size              = static_cast<int>(read_size);
    config.disable_auto_redirect    = true;
    esp_err_t err                   = transport_http_configure_tls(&config, request->url);
    if (err != ESP_OK)
    {
        return err;
    }
    TransportHttpClient client(config);
    if (client.get() == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    err = ESP_OK;
    for (size_t i = 0; err == ESP_OK && i < request->header_count; ++i)
    {
        if (request->headers[i].name == nullptr || request->headers[i].value == nullptr)
        {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = esp_http_client_set_header(client.get(), request->headers[i].name, request->headers[i].value);
    }
    if (err == ESP_OK)
    {
        err = transport_http_open_download_response(&client, request->automatic_redirects, result);
    }

    TransportHttpHeapPtr<uint8_t> buffer;
    if (err == ESP_OK)
    {
        buffer.reset(static_cast<uint8_t *>(heap_caps_malloc(read_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        if (buffer == nullptr)
        {
            err = ESP_ERR_NO_MEM;
        }
    }
    while (err == ESP_OK)
    {
        if (request->should_continue != nullptr && !request->should_continue(request->ctx))
        {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        const int received =
            esp_http_client_read(client.get(), reinterpret_cast<char *>(buffer.get()), static_cast<int>(read_size));
        if (received < 0)
        {
            err = ESP_FAIL;
            break;
        }
        if (received == 0)
        {
            break;
        }
        result->received_bytes += static_cast<size_t>(received);
        err = request->on_response_data(buffer.get(), static_cast<size_t>(received), request->ctx);
    }
    if (err == ESP_OK && result->content_length >= 0
        && result->received_bytes != static_cast<size_t>(result->content_length))
    {
        err = ESP_ERR_INVALID_SIZE;
    }
    return err;
}
