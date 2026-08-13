/**
 * @file web_console_service_http.cpp
 * @brief 网页控制台 Service 的 HTTP 路由、首页与认证会话实现
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "web_console_http_common.hpp"
#include "web_console_service_internal.hpp"
#include "web_console_service_web.h"

#if CONFIG_WEB_CONSOLE_FILES
#include "web_console_files_internal.hpp"
#endif
#if CONFIG_WEB_CONSOLE_SETTINGS || CONFIG_WEB_CONSOLE_STATUS || CONFIG_WEB_CONSOLE_ACTIONS
#include "web_console_provider_internal.hpp"
#endif

#define WEB_FILE_TOKEN_RESPONSE_SIZE 45U

static const char *TAG = "web_console_service";

enum web_file_session_body_result_t
{
    WEB_FILE_SESSION_BODY_OK = 0,
    WEB_FILE_SESSION_BODY_MALFORMED,
    WEB_FILE_SESSION_BODY_IO_FAILED,
};

static TickType_t web_file_remaining_wait_ticks(int64_t deadline_us);

static void web_file_secure_clear(void *data, size_t size)
{
    volatile uint8_t *cursor = static_cast<volatile uint8_t *>(data);
    while (size > 0U)
    {
        *cursor = 0U;
        ++cursor;
        --size;
    }
}

/**
 * @brief 向客户端发送 503 Service Unavailable 错误响应
 *
 * 当 Service 正在停止、不再接纳请求时，由各 handler 调用此函数通知客户端。
 *
 * @param[in] request HTTPD 在本次回调期间借出的请求对象
 * @return ESP_OK 响应发送完成；其他错误码来自 HTTPD 响应 API
 */
static esp_err_t web_file_send_unavailable(httpd_req_t *request)
{
    return web_console_http_send_json_error(request,
                                            "503 Service Unavailable",
                                            "{\"error\":\"service_unavailable\",\"message\":\"服务正在停止\"}");
}

bool web_console_handler_enter(void)
{
    SemaphoreHandle_t lock = s_context.lock;
    if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    ++s_context.active_handlers;
    const bool accepting = s_context.accepting_requests && s_context.state == WEB_CONSOLE_SERVICE_STATE_RUNNING;
    xSemaphoreGive(lock);
    return accepting;
}

void web_console_handler_leave(void)
{
    SemaphoreHandle_t lock = s_context.lock;
    if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    bool signal_drained  = false;
    bool count_underflow = false;
    if (s_context.active_handlers == 0U)
    {
        count_underflow = true;
    }
    else
    {
        --s_context.active_handlers;
        signal_drained = s_context.active_handlers == 0U;
    }
    SemaphoreHandle_t handlers_drained = s_context.handlers_drained;
    xSemaphoreGive(lock);

    if (signal_drained && handlers_drained != NULL)
    {
        xSemaphoreGive(handlers_drained);
    }
    if (count_underflow)
    {
        ESP_LOGE(TAG, "HTTP handler 活动计数发生下溢");
    }
}

/**
 * @brief 在 handler 记账范围内发送 flash 中的 gzip 首页
 *
 * 统一 dispatcher 已在调用本函数前完成 handler 记账，因此停止流程可以安全等待本次
 * flash 读取和网络发送结束。
 *
 * @param[in] request HTTPD 在本次回调期间借出的请求对象
 * @return ESP_OK 响应发送完成；ESP_FAIL 入口关闭后拒绝请求且未发送响应；
 *         其他错误码来自 HTTPD 响应 API
 */
static esp_err_t handle_index_get(httpd_req_t *request)
{
    esp_err_t error = httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (error == ESP_OK)
    {
        error = httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    }
    if (error == ESP_OK)
    {
        error = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    }
    if (error == ESP_OK)
    {
        error = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    }
    if (error == ESP_OK)
    {
        error = httpd_resp_send(request,
                                (const char *) web_console_index_gz,
                                (ssize_t) web_console_index_gz_size);
    }

    return error;
}

/**
 * @brief 检查请求的 Content-Type 是否为 text/plain
 *
 * 从 HTTP 请求头部读取 Content-Type 值，以大小写不敏感方式比较前缀。头部缺失或读取
 * 失败（截断除外）时返回 false。
 *
 * @param[in] request HTTPD 在本次回调期间借出的请求对象
 * @return true Content-Type 以 "text/plain" 开头；false 否则或头部不可用
 */
static bool web_file_content_type_is_text_plain(httpd_req_t *request)
{
    static const char prefix[] = "text/plain";
    char value[sizeof(prefix) + 1U]{};

    const esp_err_t error = httpd_req_get_hdr_value_str(request, "Content-Type", value, sizeof(value));
    if (error != ESP_OK && error != ESP_ERR_HTTPD_RESULT_TRUNC)
    {
        return false;
    }
    return strncasecmp(value, prefix, sizeof(prefix) - 1U) == 0
           && (value[sizeof(prefix) - 1U] == '\0'
               || value[sizeof(prefix) - 1U] == ';');
}

/**
 * @brief 接收会话端点的固定六字节访问码正文
 *
 * HTTPD 的 recv 可能返回部分数据，因此函数循环到六字节完整到达。任何 recv 错误单独标记为
 * I/O 失败，调用方发送 400 后仍需返回错误，让 HTTPD 关闭异常连接。输出缓冲区属于调用方，
 * 无论成功、格式错误还是 I/O 失败，调用方都必须在离开 handler 前清除其中可能残留的访问码。
 *
 * @param[in] request HTTPD 在本次回调期间借出的请求对象
 * @param[out] out_code 调用方提供的七字节访问码缓冲区，成功时写入 NUL 结尾字符串
 * @return 正文解析结果
 */
static web_file_session_body_result_t web_file_receive_access_code(httpd_req_t *request, char out_code[7])
{
    if (request->content_len != WEB_FILE_ACCESS_CODE_LENGTH || !web_file_content_type_is_text_plain(request))
    {
        return WEB_FILE_SESSION_BODY_MALFORMED;
    }

    size_t received = 0U;
    while (received < WEB_FILE_ACCESS_CODE_LENGTH)
    {
        const int chunk = httpd_req_recv(request, out_code + received, WEB_FILE_ACCESS_CODE_LENGTH - received);
        if (chunk <= 0)
        {
            return WEB_FILE_SESSION_BODY_IO_FAILED;
        }
        received += (size_t) chunk;
    }
    out_code[WEB_FILE_ACCESS_CODE_LENGTH] = '\0';
    return WEB_FILE_SESSION_BODY_OK;
}

/**
 * @brief 在认证状态副本上探测访问码并提交失败计数
 *
 * 探测成功只修改栈上副本，不会把占位 token 发布到活动认证状态；这样调用方可以在确认访问码
 * 有效后才生成 128 位随机 token。失败或锁定结果会在同一 Service 锁范围内提交，保持尝试
 * 计数和锁定期限串行一致。
 *
 * @param[in] code 六位访问码
 * @param[in] now_us 当前单调时间
 * @param[out] out_service_available 当前服务是否仍接纳认证提交
 * @return 访问码探测结果
 */
static web_file_auth_result_t web_file_probe_access_code(const char code[7], int64_t now_us,
                                                         bool *out_service_available)
{
    web_file_auth_state_t  candidate{};
    uint8_t                placeholder_token[WEB_FILE_TOKEN_BYTES]{};
    char                   placeholder_text[WEB_FILE_TOKEN_BUFFER_SIZE]{};
    web_file_auth_result_t result                                       = WEB_FILE_AUTH_UNAUTHORIZED;

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    *out_service_available = s_context.accepting_requests && s_context.state == WEB_CONSOLE_SERVICE_STATE_RUNNING;
    if (*out_service_available)
    {
        candidate = s_context.auth;
        result    = web_file_auth_create_session(&candidate, code, placeholder_token, now_us, placeholder_text);
        if (result != WEB_FILE_AUTH_OK)
        {
            s_context.auth = candidate;
        }
    }
    xSemaphoreGive(s_context.lock);

    web_file_secure_clear(&candidate, sizeof(candidate));
    web_file_secure_clear(placeholder_token, sizeof(placeholder_token));
    web_file_secure_clear(placeholder_text, sizeof(placeholder_text));
    return result;
}

/**
 * @brief 在 Service 锁内提交已生成的认证 token
 *
 * 提交前再次检查接纳状态，防止 stop 在访问码探测与随机数生成之间清除秘密后，旧请求重新创建
 * 会话。成功时 token 同时写入锁内认证状态和调用方缓冲区；调用方缓冲区只借给本函数，调用方
 * 必须在构造完响应后覆盖。
 *
 * @param[in] code 六位访问码
 * @param[in] random_token 已生成的 128 位随机 token
 * @param[in] now_us 当前单调时间
 * @param[out] out_token 仅在返回 `WEB_FILE_AUTH_OK` 时有效的 32 位十六进制 token
 * @param[out] out_service_available 当前服务是否仍接纳认证提交
 * @return 会话创建结果
 */
static web_file_auth_result_t web_file_commit_session(const char    code[7],
                                                      const uint8_t random_token[WEB_FILE_TOKEN_BYTES], int64_t now_us,
                                                      char  out_token[WEB_FILE_TOKEN_BUFFER_SIZE],
                                                      bool *out_service_available)
{
    web_file_auth_result_t result = WEB_FILE_AUTH_UNAUTHORIZED;

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    *out_service_available = s_context.accepting_requests && s_context.state == WEB_CONSOLE_SERVICE_STATE_RUNNING;
    if (*out_service_available)
    {
        result = web_file_auth_create_session(&s_context.auth, code, random_token, now_us, out_token);
    }
    xSemaphoreGive(s_context.lock);
    return result;
}

/**
 * @brief 响应发送失败时撤销本次刚提交的认证会话
 *
 * 仅当当前活动会话的二进制 token 与本次提交值完全一致时才撤销，避免停止流程已经清空秘密
 * 或未来其他会话替换状态后误删新会话。这里的比较只用于失败清理，不承担认证判定。
 *
 * @param[in] expected_token 本次会话提交使用的 128 位随机 token
 */
static void web_file_revoke_session_if_token_matches(const uint8_t expected_token[WEB_FILE_TOKEN_BYTES])
{
    char access_code[WEB_FILE_ACCESS_CODE_BUFFER_SIZE]{};

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    if (s_context.auth.session_active && memcmp(s_context.auth.token, expected_token, WEB_FILE_TOKEN_BYTES) == 0)
    {
        memcpy(access_code, s_context.auth.access_code, sizeof(access_code));
        web_file_auth_reset(&s_context.auth, access_code);
    }
    xSemaphoreGive(s_context.lock);

    web_file_secure_clear(access_code, sizeof(access_code));
}

static esp_err_t web_file_send_auth_result(httpd_req_t *request, web_file_auth_result_t result)
{
    switch (result)
    {
        case WEB_FILE_AUTH_BAD_CODE:
        case WEB_FILE_AUTH_UNAUTHORIZED:
        case WEB_FILE_AUTH_EXPIRED:
            return web_console_http_send_json_error(request,
                                                    "401 Unauthorized",
                                                    "{\"error\":\"unauthorized\",\"message\":\"访问码错误\"}");
        case WEB_FILE_AUTH_LOCKED:
            return web_console_http_send_json_error(request,
                                                    "423 Locked",
                                                    "{\"error\":\"locked\",\"message\":\"访问码尝试次数过多，请稍后重试\"}");
        case WEB_FILE_AUTH_SESSION_BUSY:
            return web_console_http_send_json_error(request,
                                                    "409 Conflict",
                                                    "{\"error\":\"session_conflict\",\"message\":\"已有活动会话\"}");
        case WEB_FILE_AUTH_OK:
        default:
            return web_console_http_send_json_error(request,
                                                    "500 Internal Server Error",
                                                    "{\"error\":\"internal_error\",\"message\":\"服务内部错误\"}");
    }
}

/**
 * @brief 校验六位访问码并创建唯一认证会话
 *
 * 本函数先在认证状态副本上探测访问码，只有探测成功才生成随机 token；提交时再次在 Service
 * 锁内检查请求接纳状态，避免与 `stop()` 清除秘密的动作交错后重新发布会话。访问码、二进制
 * token、十六进制 token 和 JSON 响应中的 token 均在各自最后一次使用后于栈上覆盖。每条
 * 返回路径都与入口的 handler 记账配对。成功提交后的响应若构造或发送失败，只在 token
 * 仍匹配本次提交值时撤销会话，避免客户端未收到凭据却持续收到会话冲突。
 *
 * @param[in] request HTTPD 在本次回调期间借出的请求对象
 * @return ESP_OK 响应发送完成；ESP_FAIL 入口关闭后拒绝请求或请求正文接收失败；
 *         其他错误码来自 HTTPD 响应 API
 */
static esp_err_t handle_session_post(httpd_req_t *request)
{
    esp_err_t error;
    char      code[WEB_FILE_ACCESS_CODE_BUFFER_SIZE]{};

    const web_file_session_body_result_t body_result = web_file_receive_access_code(request, code);
    if (body_result != WEB_FILE_SESSION_BODY_OK)
    {
        error = web_console_http_send_json_error(request,
                                                 "400 Bad Request",
                                                 "{\"error\":\"bad_request\",\"message\":\"请求正文必须是六字节访问码\"}");
        web_file_secure_clear(code, sizeof(code));
        return body_result == WEB_FILE_SESSION_BODY_IO_FAILED ? ESP_FAIL : error;
    }

    const int64_t          now_us = esp_timer_get_time();
    bool                   service_available;
    web_file_auth_result_t auth_result = web_file_probe_access_code(code, now_us, &service_available);
    if (!service_available)
    {
        web_file_secure_clear(code, sizeof(code));
        error = web_file_send_unavailable(request);
        return error;
    }
    if (auth_result != WEB_FILE_AUTH_OK)
    {
        web_file_secure_clear(code, sizeof(code));
        error = web_file_send_auth_result(request, auth_result);
        return error;
    }

    uint8_t random_token[WEB_FILE_TOKEN_BYTES];
    char    token[WEB_FILE_TOKEN_BUFFER_SIZE]{};
    esp_fill_random(random_token, sizeof(random_token));
    auth_result = web_file_commit_session(code, random_token, now_us, token, &service_available);
    web_file_secure_clear(code, sizeof(code));

    if (!service_available)
    {
        web_file_secure_clear(random_token, sizeof(random_token));
        web_file_secure_clear(token, sizeof(token));
        error = web_file_send_unavailable(request);
        return error;
    }
    if (auth_result != WEB_FILE_AUTH_OK)
    {
        web_file_secure_clear(random_token, sizeof(random_token));
        web_file_secure_clear(token, sizeof(token));
        error = web_file_send_auth_result(request, auth_result);
        return error;
    }

    char      response[WEB_FILE_TOKEN_RESPONSE_SIZE];
    const int response_size = snprintf(response, sizeof(response), "{\"token\":\"%s\"}", token);
    web_file_secure_clear(token, sizeof(token));
    if (response_size < 0 || (size_t) response_size >= sizeof(response))
    {
        web_file_revoke_session_if_token_matches(random_token);
        web_file_secure_clear(random_token, sizeof(random_token));
        web_file_secure_clear(response, sizeof(response));
        error = web_console_http_send_json_error(request,
                                                 "500 Internal Server Error",
                                                 "{\"error\":\"internal_error\",\"message\":\"服务内部错误\"}");
        return error;
    }

    error = web_console_http_set_json_response(request, "200 OK");
    if (error == ESP_OK)
    {
        error = httpd_resp_send(request, response, (ssize_t) response_size);
    }
    if (error != ESP_OK)
    {
        web_file_revoke_session_if_token_matches(random_token);
    }
    web_file_secure_clear(random_token, sizeof(random_token));
    web_file_secure_clear(response, sizeof(response));
    return error;
}

/** @brief 校验当前 token 并关闭唯一认证会话。 */
static esp_err_t handle_session_delete(httpd_req_t *request)
{
    const web_console_http_auth_result_t auth_result =
        web_console_http_close_session(request);
    if (auth_result != WEB_CONSOLE_HTTP_AUTH_OK)
    {
        return web_console_http_send_auth_error(request, auth_result);
    }

    esp_err_t error = web_console_http_set_json_response(request, "204 No Content");
    if (error == ESP_OK)
    {
        error = httpd_resp_send(request, NULL, 0U);
    }
    return error;
}

/** @brief 返回本次构建及实际装配的网页模块描述。 */
static esp_err_t handle_capabilities_get(httpd_req_t *request)
{
#if CONFIG_WEB_CONSOLE_SETTINGS || CONFIG_WEB_CONSOLE_STATUS || CONFIG_WEB_CONSOLE_ACTIONS
    return web_console_provider_handle_capabilities_get(request);
#else
    const web_console_http_auth_result_t auth_result =
        web_console_http_authorize_request(request);
    if (auth_result != WEB_CONSOLE_HTTP_AUTH_OK)
    {
        return web_console_http_send_auth_error(request, auth_result);
    }
#if CONFIG_WEB_CONSOLE_FILES
    static const char body[] =
        "{\"schema\":1,\"modules\":[{\"id\":\"files\",\"label\":\"文件\"}]}";
#else
    static const char body[] = "{\"schema\":1,\"modules\":[]}";
#endif
    return web_console_http_send_json(request, "200 OK", body, sizeof(body) - 1U);
#endif
}

struct web_console_route_slot_t
{
    web_console_route_t route;
    bool                registered;
};

static constexpr web_console_route_t s_core_routes[] = {
    { .uri = "/", .method = HTTP_GET, .handle = handle_index_get },
    { .uri = "/api/session", .method = HTTP_POST, .handle = handle_session_post },
    { .uri = "/api/session", .method = HTTP_DELETE, .handle = handle_session_delete },
    { .uri = "/api/capabilities", .method = HTTP_GET, .handle = handle_capabilities_get },
};

/*
 * HTTPD 只浅拷贝 user_ctx，因此固定槽必须覆盖注册、部分注销、停止以及失败重试的完整生命周期。
 * 槽内容只会在没有任何已注册路由时重建，不使用可重分配容器。
 */
static web_console_route_slot_t s_route_slots[WEB_CONSOLE_ROUTE_COUNT]{};

class web_console_handler_scope_t
{
  public:
    web_console_handler_scope_t() : accepting_(web_console_handler_enter()) {}
    web_console_handler_scope_t(const web_console_handler_scope_t &)            = delete;
    web_console_handler_scope_t &operator=(const web_console_handler_scope_t &) = delete;

    ~web_console_handler_scope_t()
    {
        web_console_handler_leave();
    }

    bool accepting(void) const
    {
        return accepting_;
    }

  private:
    bool accepting_;
};

/**
 * @brief 统一完成全部领域路由的准入、handler 记账与退出配对
 */
static esp_err_t web_console_http_dispatch(httpd_req_t *request)
{
    web_console_handler_scope_t scope;
    if (!scope.accepting())
    {
        return ESP_FAIL;
    }
    if (request == NULL || request->user_ctx == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const web_console_route_t *route = static_cast<const web_console_route_t *>(request->user_ctx);
    if (route->handle == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return route->handle(request);
}

static bool web_console_routes_collide(const web_console_route_t &left, const web_console_route_t &right)
{
    return left.method == right.method && strcmp(left.uri, right.uri) == 0;
}

/**
 * @brief 在尚无已注册入口时装配 Core、Files 与 Provider 的固定路由槽
 */
static esp_err_t web_console_prepare_route_slots(void)
{
#if CONFIG_WEB_CONSOLE_FILES
    size_t files_route_count = 0U;
    const web_console_route_t *files_routes = web_console_files_get_routes(&files_route_count);
    if (files_routes == NULL || files_route_count != WEB_CONSOLE_FILES_ROUTE_COUNT)
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif
#if CONFIG_WEB_CONSOLE_SETTINGS || CONFIG_WEB_CONSOLE_STATUS || CONFIG_WEB_CONSOLE_ACTIONS
    size_t provider_route_count = 0U;
    const web_console_route_t *provider_routes =
        web_console_provider_get_routes(&provider_route_count);
    if (provider_routes == NULL
        || provider_route_count != WEB_CONSOLE_PROVIDER_ROUTE_COUNT)
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif

    for (size_t index = 0U; index < WEB_CONSOLE_CORE_ROUTE_COUNT; ++index)
    {
        s_route_slots[index].route      = s_core_routes[index];
        s_route_slots[index].registered = false;
    }
#if CONFIG_WEB_CONSOLE_FILES
    for (size_t index = 0U; index < files_route_count; ++index)
    {
        const size_t slot_index = WEB_CONSOLE_CORE_ROUTE_COUNT + index;
        s_route_slots[slot_index].route      = files_routes[index];
        s_route_slots[slot_index].registered = false;
    }
#endif
#if CONFIG_WEB_CONSOLE_SETTINGS || CONFIG_WEB_CONSOLE_STATUS || CONFIG_WEB_CONSOLE_ACTIONS
    for (size_t index = 0U; index < provider_route_count; ++index)
    {
        const size_t slot_index =
            WEB_CONSOLE_CORE_ROUTE_COUNT + WEB_CONSOLE_FILES_ROUTE_COUNT + index;
        s_route_slots[slot_index].route      = provider_routes[index];
        s_route_slots[slot_index].registered = false;
    }
#endif

    for (size_t index = 0U; index < WEB_CONSOLE_ROUTE_COUNT; ++index)
    {
        const web_console_route_t &route = s_route_slots[index].route;
        if (route.uri == NULL || route.handle == NULL)
        {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t compared = 0U; compared < index; ++compared)
        {
            if (web_console_routes_collide(route, s_route_slots[compared].route))
            {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    return ESP_OK;
}

/**
 * @brief 在 HTTPD Task 上注销本 Service 已注册的全部 URI handler
 *
 * HTTPD 的 URI 表只能由其所属 Task 串行修改。本回调逐项记录成功注销的 handler，并在锁内
 * 发布每个固定槽的注册状态和首个错误；完成信号只负责唤醒等待方，全部槽清空才是入口关闭
 * 的完成条件。
 *
 * @param[in] argument Service 当前持有的 HTTPD 句柄
 */
static void web_file_unregister_handlers_work(void *argument)
{
    httpd_handle_t server = static_cast<httpd_handle_t>(argument);

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    bool registered[WEB_CONSOLE_ROUTE_COUNT]{};
    for (size_t index = 0U; index < WEB_CONSOLE_ROUTE_COUNT; ++index)
    {
        registered[index] = s_route_slots[index].registered;
    }
    xSemaphoreGive(s_context.lock);

    esp_err_t first_error = ESP_OK;

    for (size_t index = 0U; index < WEB_CONSOLE_ROUTE_COUNT; ++index)
    {
        if (!registered[index])
        {
            continue;
        }

        const web_console_route_t &route = s_route_slots[index].route;
        const esp_err_t error = httpd_unregister_uri_handler(server, route.uri, route.method);
        if (error == ESP_OK || error == ESP_ERR_NOT_FOUND)
        {
            xSemaphoreTake(s_context.lock, portMAX_DELAY);
            if (s_route_slots[index].registered)
            {
                s_route_slots[index].registered = false;
                if (s_context.registered_route_count > 0U)
                {
                    --s_context.registered_route_count;
                }
            }
            xSemaphoreGive(s_context.lock);
        }
        else if (first_error == ESP_OK)
        {
            first_error = error;
        }
    }

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    s_context.ingress_close_error     = first_error;
    s_context.ingress_close_queued    = false;
    SemaphoreHandle_t ingress_closed  = s_context.ingress_closed;
    xSemaphoreGive(s_context.lock);

    if (ingress_closed != NULL)
    {
        xSemaphoreGive(ingress_closed);
    }
}

static_assert(sizeof(s_core_routes) / sizeof(s_core_routes[0]) == WEB_CONSOLE_CORE_ROUTE_COUNT,
              "Core 路由数量必须与固定槽配置一致");

bool web_console_http_routes_are_released_locked(void)
{
    if (s_context.registered_route_count != 0U)
    {
        return false;
    }
    for (size_t index = 0U; index < WEB_CONSOLE_ROUTE_COUNT; ++index)
    {
        if (s_route_slots[index].registered)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 注册网页控制台 Service 的全部 HTTP URI
 *
 * @param[in] server 已启动且由 Service 持有的 HTTPD 句柄
 * @return ESP_OK 全部注册完成；其他错误码来自 HTTPD
 */
esp_err_t web_console_http_register_handlers(httpd_handle_t server)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    const bool clean = web_console_http_routes_are_released_locked();
    xSemaphoreGive(s_context.lock);
    if (!clean)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = web_console_prepare_route_slots();
    for (size_t index = 0U; error == ESP_OK && index < WEB_CONSOLE_ROUTE_COUNT; ++index)
    {
        web_console_route_slot_t &slot = s_route_slots[index];
        const httpd_uri_t uri = {
            .uri      = slot.route.uri,
            .method   = slot.route.method,
            .handler  = web_console_http_dispatch,
            .user_ctx = &slot.route,
        };
        error = httpd_register_uri_handler(server, &uri);
        if (error == ESP_OK)
        {
            xSemaphoreTake(s_context.lock, portMAX_DELAY);
            slot.registered = true;
            ++s_context.registered_route_count;
            xSemaphoreGive(s_context.lock);
        }
    }
    return error;
}

esp_err_t web_console_http_close_ingress(httpd_handle_t server, int64_t deadline_us)
{
    bool queue_work = false;

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    if (web_console_http_routes_are_released_locked())
    {
        xSemaphoreGive(s_context.lock);
        return ESP_OK;
    }
    if (!s_context.ingress_close_queued)
    {
        s_context.ingress_close_queued = true;
        s_context.ingress_close_error  = ESP_OK;
        queue_work                     = true;
    }
    xSemaphoreGive(s_context.lock);

    if (queue_work)
    {
        const esp_err_t error = httpd_queue_work(server, web_file_unregister_handlers_work, server);
        if (error != ESP_OK)
        {
            xSemaphoreTake(s_context.lock, portMAX_DELAY);
            s_context.ingress_close_queued = false;
            s_context.ingress_close_error  = error;
            xSemaphoreGive(s_context.lock);
            return error;
        }
    }

    while (true)
    {
        xSemaphoreTake(s_context.lock, portMAX_DELAY);
        const bool              routes_released = web_console_http_routes_are_released_locked();
        const bool              close_queued    = s_context.ingress_close_queued;
        const esp_err_t         close_error     = s_context.ingress_close_error;
        const SemaphoreHandle_t ingress_closed  = s_context.ingress_closed;
        xSemaphoreGive(s_context.lock);

        if (routes_released)
        {
            return ESP_OK;
        }
        if (!close_queued)
        {
            return close_error == ESP_OK ? ESP_FAIL : close_error;
        }

        const TickType_t wait_ticks = web_file_remaining_wait_ticks(deadline_us);
        if (wait_ticks == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }
        (void) xSemaphoreTake(ingress_closed, wait_ticks);
    }
}

static TickType_t web_file_remaining_wait_ticks(int64_t deadline_us)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us >= deadline_us)
    {
        return 0U;
    }

    const uint64_t remaining_ms = (uint64_t) (deadline_us - now_us) / 1000U;
    uint64_t       wait_ticks   = remaining_ms * (uint64_t) configTICK_RATE_HZ / 1000U;
    if (wait_ticks >= (uint64_t) portMAX_DELAY)
    {
        wait_ticks = (uint64_t) portMAX_DELAY - 1U;
    }
    return (TickType_t) wait_ticks;
}
