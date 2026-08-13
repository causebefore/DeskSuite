/**
 * @file web_console_http_common.cpp
 * @brief 网页控制台认证后 HTTP 路由共用的响应与会话校验实现
 */
#include "web_console_http_common.hpp"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"
#include "web_console_service_internal.hpp"

#define WEB_CONSOLE_HTTP_BEARER_PREFIX              "Bearer "
#define WEB_CONSOLE_HTTP_AUTHORIZATION_BUFFER_SIZE \
    (sizeof(WEB_CONSOLE_HTTP_BEARER_PREFIX) - 1U + WEB_FILE_TOKEN_BUFFER_SIZE)

/** @brief 以不会被编译器省略的逐字节写入清除认证材料。 */
static void web_console_http_secure_clear(void *data, size_t size)
{
    volatile uint8_t *cursor = static_cast<volatile uint8_t *>(data);
    while (size > 0U)
    {
        *cursor = 0U;
        ++cursor;
        --size;
    }
}

/** @brief 严格读取固定长度 Bearer token；失败时输出保持全零。 */
static bool web_console_http_read_bearer(
    httpd_req_t *request,
    char out_authorization[WEB_CONSOLE_HTTP_AUTHORIZATION_BUFFER_SIZE])
{
    if (request == NULL)
    {
        return false;
    }

    const size_t authorization_size = httpd_req_get_hdr_value_len(request, "Authorization");
    if (authorization_size != sizeof(WEB_CONSOLE_HTTP_BEARER_PREFIX) - 1U + WEB_FILE_TOKEN_BUFFER_SIZE - 1U
        || authorization_size >= WEB_CONSOLE_HTTP_AUTHORIZATION_BUFFER_SIZE
        || httpd_req_get_hdr_value_str(
               request, "Authorization", out_authorization, WEB_CONSOLE_HTTP_AUTHORIZATION_BUFFER_SIZE)
               != ESP_OK
        || memcmp(out_authorization,
                  WEB_CONSOLE_HTTP_BEARER_PREFIX,
                  sizeof(WEB_CONSOLE_HTTP_BEARER_PREFIX) - 1U)
               != 0)
    {
        return false;
    }
    return true;
}

web_console_http_auth_result_t web_console_http_authorize_request(httpd_req_t *request)
{
    char authorization[WEB_CONSOLE_HTTP_AUTHORIZATION_BUFFER_SIZE]{};
    if (!web_console_http_read_bearer(request, authorization))
    {
        web_console_http_secure_clear(authorization, sizeof(authorization));
        return WEB_CONSOLE_HTTP_AUTH_UNAUTHORIZED;
    }

    web_console_http_auth_result_t result = WEB_CONSOLE_HTTP_AUTH_UNAUTHORIZED;
    SemaphoreHandle_t lock = s_context.lock;
    if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
    {
        web_console_http_secure_clear(authorization, sizeof(authorization));
        return WEB_CONSOLE_HTTP_AUTH_UNAVAILABLE;
    }

    if (!s_context.accepting_requests || s_context.state != WEB_CONSOLE_SERVICE_STATE_RUNNING)
    {
        result = WEB_CONSOLE_HTTP_AUTH_UNAVAILABLE;
    }
    else if (web_file_auth_authorize(&s_context.auth,
                                     authorization + sizeof(WEB_CONSOLE_HTTP_BEARER_PREFIX) - 1U,
                                     esp_timer_get_time(),
                                     false)
             == WEB_FILE_AUTH_OK)
    {
        result = WEB_CONSOLE_HTTP_AUTH_OK;
    }
    xSemaphoreGive(lock);

    web_console_http_secure_clear(authorization, sizeof(authorization));
    return result;
}

web_console_http_auth_result_t web_console_http_close_session(httpd_req_t *request)
{
    char authorization[WEB_CONSOLE_HTTP_AUTHORIZATION_BUFFER_SIZE]{};
    if (!web_console_http_read_bearer(request, authorization))
    {
        web_console_http_secure_clear(authorization, sizeof(authorization));
        return WEB_CONSOLE_HTTP_AUTH_UNAUTHORIZED;
    }

    web_console_http_auth_result_t result = WEB_CONSOLE_HTTP_AUTH_UNAUTHORIZED;
    char access_code[WEB_FILE_ACCESS_CODE_BUFFER_SIZE]{};
    SemaphoreHandle_t lock = s_context.lock;
    if (lock == NULL || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE)
    {
        web_console_http_secure_clear(authorization, sizeof(authorization));
        return WEB_CONSOLE_HTTP_AUTH_UNAVAILABLE;
    }

    if (!s_context.accepting_requests || s_context.state != WEB_CONSOLE_SERVICE_STATE_RUNNING)
    {
        result = WEB_CONSOLE_HTTP_AUTH_UNAVAILABLE;
    }
    else if (web_file_auth_authorize(&s_context.auth,
                                     authorization + sizeof(WEB_CONSOLE_HTTP_BEARER_PREFIX) - 1U,
                                     esp_timer_get_time(),
                                     false)
             == WEB_FILE_AUTH_OK)
    {
        memcpy(access_code, s_context.auth.access_code, sizeof(access_code));
        web_file_auth_reset(&s_context.auth, access_code);
        result = WEB_CONSOLE_HTTP_AUTH_OK;
    }
    xSemaphoreGive(lock);

    web_console_http_secure_clear(access_code, sizeof(access_code));
    web_console_http_secure_clear(authorization, sizeof(authorization));
    return result;
}

esp_err_t web_console_http_set_json_response(httpd_req_t *request, const char *status)
{
    if (request == NULL || status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = httpd_resp_set_status(request, status);
    if (error == ESP_OK)
    {
        error = httpd_resp_set_type(request, "application/json; charset=utf-8");
    }
    if (error == ESP_OK)
    {
        error = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    }
    if (error == ESP_OK)
    {
        error = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    }
    return error;
}

esp_err_t web_console_http_send_json(httpd_req_t *request,
                                     const char *status,
                                     const char *body,
                                     size_t body_size_bytes)
{
    if (body == NULL || body_size_bytes > (size_t) INT_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = web_console_http_set_json_response(request, status);
    if (error != ESP_OK)
    {
        return error;
    }
    return httpd_resp_send(request, body, (ssize_t) body_size_bytes);
}

esp_err_t web_console_http_send_json_error(httpd_req_t *request, const char *status, const char *body)
{
    if (body == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return web_console_http_send_json(request, status, body, strlen(body));
}

esp_err_t web_console_http_send_auth_error(httpd_req_t *request, web_console_http_auth_result_t result)
{
    switch (result)
    {
        case WEB_CONSOLE_HTTP_AUTH_UNAUTHORIZED:
            return web_console_http_send_json_error(
                request,
                "401 Unauthorized",
                "{\"error\":\"unauthorized\",\"message\":\"认证信息无效或已过期\"}");
        case WEB_CONSOLE_HTTP_AUTH_UNAVAILABLE:
            return web_console_http_send_json_error(
                request,
                "503 Service Unavailable",
                "{\"error\":\"service_unavailable\",\"message\":\"服务正在停止\"}");
        case WEB_CONSOLE_HTTP_AUTH_OK:
        default:
            return ESP_ERR_INVALID_STATE;
    }
}
