/**
 * @file web_console_http_common.hpp
 * @brief 网页控制台认证后 HTTP 路由共用的响应与会话校验接口
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * @brief 普通认证路由的会话校验结果
 */
enum web_console_http_auth_result_t
{
    WEB_CONSOLE_HTTP_AUTH_OK = 0,
    WEB_CONSOLE_HTTP_AUTH_UNAUTHORIZED,
    WEB_CONSOLE_HTTP_AUTH_UNAVAILABLE,
};

/**
 * @brief 校验请求携带的固定 Bearer token
 *
 * 本函数只在 Core 状态锁内检查接纳状态和认证状态，不持有锁返回。调用方后续执行 Provider
 * 回调时不得再次取得或假定持有 Core 锁。
 *
 * @param[in] request HTTPD 在当前 handler 期间借出的请求
 * @return 会话校验结果
 */
web_console_http_auth_result_t web_console_http_authorize_request(httpd_req_t *request);

/**
 * @brief 原子校验 Bearer token 并关闭当前会话
 *
 * 成功时保留本次运行的六位访问码，使同一浏览器或另一浏览器可以重新登录；token 和会话
 * 活动时间在 Core 锁内清除。
 *
 * @param[in] request HTTPD 在当前 handler 期间借出的请求
 * @return 会话校验结果
 */
web_console_http_auth_result_t web_console_http_close_session(httpd_req_t *request);

/**
 * @brief 设置统一的无缓存 JSON 响应头
 *
 * @param[in] request HTTPD 在当前 handler 期间借出的请求
 * @param[in] status HTTP 状态文本
 * @return ESP_OK 已设置；其他错误码来自 HTTPD
 */
esp_err_t web_console_http_set_json_response(httpd_req_t *request, const char *status);

/**
 * @brief 发送已经完成 JSON 编码的有界响应正文
 *
 * @param[in] request HTTPD 在当前 handler 期间借出的请求
 * @param[in] status HTTP 状态文本
 * @param[in] body 已编码 JSON 字节
 * @param[in] body_size_bytes 正文字节数
 * @return ESP_OK 已发送；其他错误码来自 HTTPD
 */
esp_err_t web_console_http_send_json(httpd_req_t *request,
                                     const char *status,
                                     const char *body,
                                     size_t body_size_bytes);

/**
 * @brief 发送固定 JSON 错误正文
 *
 * `body` 必须是编译期固定或已经安全编码的完整 JSON，不得传入 Provider 返回的自由文本。
 *
 * @param[in] request HTTPD 在当前 handler 期间借出的请求
 * @param[in] status HTTP 状态文本
 * @param[in] body 完整 JSON 错误正文
 * @return ESP_OK 已发送；其他错误码来自 HTTPD
 */
esp_err_t web_console_http_send_json_error(httpd_req_t *request, const char *status, const char *body);

/**
 * @brief 把普通认证失败映射为固定 JSON 错误
 *
 * @param[in] request HTTPD 在当前 handler 期间借出的请求
 * @param[in] result 会话校验结果
 * @return ESP_OK 已发送；其他错误码来自 HTTPD
 */
esp_err_t web_console_http_send_auth_error(httpd_req_t *request, web_console_http_auth_result_t result);
