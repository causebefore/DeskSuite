/**
 * @file web_console_service_transfer.cpp
 * @brief 网页文件服务的公共传输控制与上传实现
 */
#include "web_console_service_transfer.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

/**
 * @brief 覆盖栈上的认证头副本
 *
 * @param[out] data 待覆盖内存
 * @param[in] size 内存长度
 */
static void web_file_secure_clear(void *data, size_t size)
{
    volatile uint8_t *cursor = static_cast<volatile uint8_t *>(data);
    while (size > 0U)
    {
        *cursor++ = 0U;
        --size;
    }
}

/**
 * @brief 设置 UTF-8 JSON 响应的通用安全头
 *
 * @param[in] request HTTP 请求
 * @param[in] status HTTP 状态文本
 * @return ESP_OK 成功；其他错误码来自 HTTPD
 */
esp_err_t web_file_set_json_response(httpd_req_t *request, const char *status)
{
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

/**
 * @brief 发送固定 JSON 错误响应
 *
 * @param[in] request HTTP 请求
 * @param[in] status HTTP 状态文本
 * @param[in] body 固定 JSON 正文
 * @return ESP_OK 成功；其他错误码来自 HTTPD
 */
static esp_err_t web_file_send_json_error(httpd_req_t *request, const char *status, const char *body)
{
    const esp_err_t error = web_file_set_json_response(request, status);
    if (error != ESP_OK)
    {
        return error;
    }
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief 在认证成功后原子取得唯一文件传输所有权
 *
 * 本函数只在 Service 锁内访问认证和传输状态。Authorization 必须恰为大小写敏感的
 * `Bearer ` 前缀加 32 位 token；认证完成前不读取 query 或文件系统。
 *
 * @param[in] request HTTP 请求
 * @return 守卫结果
 */
web_file_guard_result_t web_file_transfer_acquire(httpd_req_t *request)
{
    char         authorization[WEB_FILE_AUTHORIZATION_BUFFER_SIZE]{};
    const size_t authorization_size = httpd_req_get_hdr_value_len(request, "Authorization");
    if (authorization_size != sizeof("Bearer ") - 1U + WEB_FILE_TOKEN_BUFFER_SIZE - 1U
        || authorization_size >= sizeof(authorization)
        || httpd_req_get_hdr_value_str(request, "Authorization", authorization, sizeof(authorization)) != ESP_OK
        || memcmp(authorization, "Bearer ", sizeof("Bearer ") - 1U) != 0)
    {
        web_file_secure_clear(authorization, sizeof(authorization));
        return WEB_FILE_GUARD_UNAUTHORIZED;
    }

    web_file_guard_result_t result = WEB_FILE_GUARD_UNAUTHORIZED;
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    if (!s_context.accepting_requests || s_context.state != WEB_CONSOLE_SERVICE_STATE_RUNNING)
    {
        result = WEB_FILE_GUARD_UNAVAILABLE;
    }
    else if (web_file_auth_authorize(&s_context.auth,
                                     authorization + sizeof("Bearer ") - 1U,
                                     esp_timer_get_time(),
                                     s_files_context.transfer_active)
             != WEB_FILE_AUTH_OK)
    {
        result = WEB_FILE_GUARD_UNAUTHORIZED;
    }
    else if (s_files_context.transfer_active)
    {
        result = WEB_FILE_GUARD_BUSY;
    }
    else
    {
        s_files_context.transfer_active        = true;
        s_files_context.active_transfer_socket = httpd_req_to_sockfd(request);
        result                           = WEB_FILE_GUARD_OK;
    }
    xSemaphoreGive(s_context.lock);

    web_file_secure_clear(authorization, sizeof(authorization));
    return result;
}

/**
 * @brief 释放当前请求持有的传输状态与可选 PSRAM 缓冲区
 */
void web_file_transfer_release(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    uint8_t *buffer                        = s_files_context.transfer_buffer;
    s_files_context.transfer_buffer        = NULL;
    s_files_context.transfer_active        = false;
    s_files_context.active_transfer_socket = -1;
    web_file_auth_touch_active_session(&s_context.auth, esp_timer_get_time());
    xSemaphoreGive(s_context.lock);

    if (buffer != NULL)
    {
        heap_caps_free(buffer);
    }
}

/**
 * @brief 把 PSRAM 缓冲区发布到 Service 传输所有权
 *
 * 停止流程只会在 handler 排空后摘除该指针，因此成功发布后由统一传输清理函数释放。
 *
 * @param[in] buffer 32 KiB PSRAM 缓冲区
 * @return true 已发布；false 传输状态不一致
 */
bool web_file_publish_transfer_buffer(uint8_t *buffer)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    const bool publish = s_files_context.transfer_active && s_files_context.transfer_buffer == NULL;
    if (publish)
    {
        s_files_context.transfer_buffer = buffer;
    }
    xSemaphoreGive(s_context.lock);
    return publish;
}

/**
 * @brief 检查停止流程是否已经取消当前传输
 *
 * @return true Service 已停止接纳请求；false 传输可继续
 */
bool web_file_transfer_is_cancelled(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    const bool cancelled = !s_context.accepting_requests || s_context.state != WEB_CONSOLE_SERVICE_STATE_RUNNING;
    xSemaphoreGive(s_context.lock);
    return cancelled;
}

/**
 * @brief 严格读取唯一 `path` query 并调用路径安全内核
 *
 * query 使用恰好 `query_size + 1` 字节的内部 RAM，禁止第二个字段或空 path。临时 query
 * 在返回前释放，输出只包含路径内核产生的规范路径。
 *
 * @param[in] request HTTP 请求
 * @param[out] logical 解码后的逻辑路径
 * @param[in] logical_size 逻辑路径缓冲区容量
 * @param[out] filesystem 映射后的文件系统路径
 * @param[in] filesystem_size 文件系统路径缓冲区容量
 * @return 操作结果
 */
web_file_operation_result_t web_file_read_and_map_path(httpd_req_t *request, char *logical, size_t logical_size,
                                                              char *filesystem, size_t filesystem_size)
{
    const size_t query_size = httpd_req_get_url_query_len(request);
    if (query_size <= sizeof("path=") - 1U || query_size > CONFIG_HTTPD_MAX_URI_LEN)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }

    char *query =
        static_cast<char *>(heap_caps_malloc(query_size + 1U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (query == NULL)
    {
        return WEB_FILE_OPERATION_NO_MEMORY;
    }

    web_file_operation_result_t result = WEB_FILE_OPERATION_BAD_REQUEST;
    if (httpd_req_get_url_query_str(request, query, query_size + 1U) == ESP_OK
        && memcmp(query, "path=", sizeof("path=") - 1U) == 0 && strchr(query, '&') == NULL)
    {
        const char *encoded_path = query + sizeof("path=") - 1U;
        if (encoded_path[0] != '\0'
            && web_file_path_decode_and_map(encoded_path, logical, logical_size, filesystem, filesystem_size) == ESP_OK)
        {
            result = WEB_FILE_OPERATION_OK;
        }
    }
    heap_caps_free(query);
    return result;
}

/**
 * @brief 严格读取固定顺序的源路径与目标路径 query
 *
 * query 必须恰为 `path=<源>&destination=<目标>`，两个值各自只执行一次百分号解码和路径
 * 安全校验。原始 `&` 不允许出现在字段值中，文件名中的 `&` 必须编码为 `%26`。
 *
 * @param[in] request HTTP 请求
 * @param[out] workspace 源路径与目标路径工作区
 * @return 操作结果
 */
web_file_operation_result_t web_file_read_and_map_move_paths(httpd_req_t               *request,
                                                                    web_file_move_workspace_t *workspace)
{
    static const char prefix[]    = "path=";
    static const char separator[] = "&destination=";

    const size_t query_size       = httpd_req_get_url_query_len(request);
    if (query_size <= sizeof(prefix) - 1U + sizeof(separator) - 1U || query_size > CONFIG_HTTPD_MAX_URI_LEN)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }

    char *query =
        static_cast<char *>(heap_caps_malloc(query_size + 1U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (query == NULL)
    {
        return WEB_FILE_OPERATION_NO_MEMORY;
    }

    web_file_operation_result_t result = WEB_FILE_OPERATION_BAD_REQUEST;
    if (httpd_req_get_url_query_str(request, query, query_size + 1U) == ESP_OK
        && memcmp(query, prefix, sizeof(prefix) - 1U) == 0)
    {
        char *encoded_source      = query + sizeof(prefix) - 1U;
        char *destination_marker  = strstr(encoded_source, separator);
        char *encoded_destination = destination_marker == NULL ? NULL : destination_marker + sizeof(separator) - 1U;
        if (destination_marker != NULL && destination_marker != encoded_source && encoded_destination[0] != '\0')
        {
            *destination_marker = '\0';
            if (strchr(encoded_source, '&') == NULL && strchr(encoded_destination, '&') == NULL
                && web_file_path_decode_and_map(encoded_source,
                                                workspace->source.logical,
                                                sizeof(workspace->source.logical),
                                                workspace->source.filesystem,
                                                sizeof(workspace->source.filesystem))
                       == ESP_OK
                && web_file_path_decode_and_map(encoded_destination,
                                                workspace->destination.logical,
                                                sizeof(workspace->destination.logical),
                                                workspace->destination.filesystem,
                                                sizeof(workspace->destination.filesystem))
                       == ESP_OK)
            {
                result = WEB_FILE_OPERATION_OK;
            }
        }
    }
    heap_caps_free(query);
    return result;
}

/**
 * @brief 严格读取并交叉核对 Content-Length
 *
 * 头值只接受无符号十进制数字；解析值必须与 HTTPD 已确认的正文长度一致，避免两个解析边界
 * 对同一请求产生不同长度。零字节上传仍必须显式携带 `Content-Length: 0`。
 *
 * @param[in] request HTTP 请求
 * @param[out] out_length 已确认的正文长度
 * @return 操作结果
 */
static web_file_operation_result_t web_file_upload_read_length(httpd_req_t *request, size_t *out_length)
{
    const size_t header_size = httpd_req_get_hdr_value_len(request, "Content-Length");
    if (header_size == 0U)
    {
        return WEB_FILE_OPERATION_LENGTH_REQUIRED;
    }
    char header[32];
    if (header_size >= sizeof(header)
        || httpd_req_get_hdr_value_str(request, "Content-Length", header, sizeof(header)) != ESP_OK)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }

    uint64_t parsed = 0U;
    for (size_t offset = 0U; offset < header_size; ++offset)
    {
        const unsigned char value = (unsigned char) header[offset];
        if (value < '0' || value > '9')
        {
            return WEB_FILE_OPERATION_BAD_REQUEST;
        }
        const uint64_t digit = (uint64_t) (value - '0');
        if (parsed > (UINT64_MAX - digit) / 10U)
        {
            return WEB_FILE_OPERATION_TOO_LARGE;
        }
        parsed = parsed * 10U + digit;
    }
    if (parsed > s_files_context.upload_max_bytes)
    {
        return WEB_FILE_OPERATION_TOO_LARGE;
    }
    if (parsed != (uint64_t) request->content_len || web_file_upload_validate_length((size_t) parsed) != ESP_OK)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }

    *out_length = (size_t) parsed;
    return WEB_FILE_OPERATION_OK;
}

/**
 * @brief 读取大小写敏感的显式覆盖确认头
 *
 * @param[in] request HTTP 请求
 * @param[out] out_confirmed 是否提供精确 `confirm`
 * @return 操作结果
 */
static web_file_operation_result_t web_file_upload_read_overwrite(httpd_req_t *request, bool *out_confirmed)
{
    const size_t header_size = httpd_req_get_hdr_value_len(request, "X-Web-Console-Overwrite");
    if (header_size == 0U)
    {
        *out_confirmed = false;
        return WEB_FILE_OPERATION_OK;
    }

    char header[sizeof("confirm")];
    if (header_size != sizeof("confirm") - 1U
        || httpd_req_get_hdr_value_str(request, "X-Web-Console-Overwrite", header, sizeof(header)) != ESP_OK
        || strcmp(header, "confirm") != 0)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }
    *out_confirmed = true;
    return WEB_FILE_OPERATION_OK;
}

/**
 * @brief 复核真实父目录与目标文件类型
 *
 * @param[in,out] workspace 已映射目标路径和父目录临时工作区
 * @param[in] overwrite_confirmed 是否明确允许覆盖
 * @param[out] out_target_exists 目标是否为已存在常规文件
 * @return 操作结果
 */
static web_file_operation_result_t web_file_upload_validate_target(web_file_transfer_workspace_t *workspace,
                                                                   bool overwrite_confirmed, bool *out_target_exists)
{
    if (strcmp(workspace->logical, "/") == 0)
    {
        return WEB_FILE_OPERATION_WRONG_TYPE;
    }

    const size_t target_size = strlen(workspace->filesystem);
    if (target_size >= sizeof(workspace->auxiliary))
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }
    memcpy(workspace->auxiliary, workspace->filesystem, target_size + 1U);
    char *separator = strrchr(workspace->auxiliary, '/');
    if (separator == NULL || separator == workspace->auxiliary)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }
    *separator = '\0';

    struct stat parent_info;
    if (stat(workspace->auxiliary, &parent_info) != 0)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    if (!S_ISDIR(parent_info.st_mode))
    {
        return WEB_FILE_OPERATION_WRONG_TYPE;
    }
    DIR *parent = opendir(workspace->auxiliary);
    if (parent == NULL)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    if (closedir(parent) != 0)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    struct stat target_info;
    if (stat(workspace->filesystem, &target_info) != 0)
    {
        if (errno == ENOENT)
        {
            *out_target_exists = false;
            return WEB_FILE_OPERATION_OK;
        }
        return errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    if (!S_ISREG(target_info.st_mode))
    {
        return WEB_FILE_OPERATION_WRONG_TYPE;
    }
    if (!overwrite_confirmed)
    {
        return WEB_FILE_OPERATION_OVERWRITE_REQUIRED;
    }

    *out_target_exists = true;
    return WEB_FILE_OPERATION_OK;
}

/**
 * @brief 要求文件系统剩余空间覆盖完整暂存文件及调用方配置的保留余量
 *
 * Files 只调用注入的容量查询，不感知调用方使用 FatFs、其他 VFS 或测试替身。
 *
 * @param[in] expected_length 上传正文长度
 * @return 操作结果
 */
static web_file_operation_result_t web_file_upload_validate_space(size_t expected_length)
{
    web_console_files_capacity_t capacity;
    if (web_console_files_get_capacity_copy(&capacity) != ESP_OK)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    const uint64_t expected_bytes = (uint64_t) expected_length;
    if (capacity.free_bytes < expected_bytes
        || capacity.free_bytes - expected_bytes < s_files_context.reserved_free_bytes)
    {
        return WEB_FILE_OPERATION_INSUFFICIENT_STORAGE;
    }
    return WEB_FILE_OPERATION_OK;
}

/**
 * @brief 独占创建、流式接收并同步固定上传临时文件
 *
 * 每个成功接收块必须由 `fwrite()` 完整消费；连续接收超时只允许固定一次重试。函数在关闭后
 * 通过 `stat()` 再次核对常规文件及精确长度。失败时调用方负责执行无 journal 的事务清理。
 *
 * @param[in] request HTTP 请求
 * @param[in] buffer 已发布给 Service 的 32 KiB PSRAM 缓冲区
 * @param[in] expected_length 预期正文长度
 * @return 操作结果
 */
static web_file_operation_result_t web_file_upload_receive_part(httpd_req_t *request, uint8_t *buffer,
                                                                size_t expected_length)
{
    const int descriptor = open(s_files_context.transaction_part, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    FILE *part = fdopen(descriptor, "wb");
    if (part == NULL)
    {
        (void) close(descriptor);
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    web_file_operation_result_t result          = WEB_FILE_OPERATION_OK;
    size_t                      received_total  = 0U;
    unsigned int                timeout_retries = 0U;
    while (received_total < expected_length)
    {
        if (web_file_transfer_is_cancelled())
        {
            result = WEB_FILE_OPERATION_CANCELLED;
            break;
        }

        const size_t remaining = expected_length - received_total;
        const size_t wanted    = remaining > WEB_FILE_TRANSFER_BUFFER_SIZE ? WEB_FILE_TRANSFER_BUFFER_SIZE : remaining;
        const int    received  = httpd_req_recv(request, (char *) buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeout_retries < WEB_FILE_UPLOAD_TIMEOUT_RETRIES)
        {
            ++timeout_retries;
            continue;
        }
        if (received <= 0 || (size_t) received > wanted)
        {
            result = web_file_transfer_is_cancelled() ? WEB_FILE_OPERATION_CANCELLED : WEB_FILE_OPERATION_IO_ERROR;
            break;
        }
        timeout_retries = 0U;
        if (fwrite(buffer, 1U, (size_t) received, part) != (size_t) received)
        {
            result = WEB_FILE_OPERATION_IO_ERROR;
            break;
        }
        received_total += (size_t) received;
    }

    if (result == WEB_FILE_OPERATION_OK && fflush(part) != 0)
    {
        result = WEB_FILE_OPERATION_IO_ERROR;
    }
    const int part_descriptor = fileno(part);
    if (result == WEB_FILE_OPERATION_OK && (part_descriptor < 0 || fsync(part_descriptor) != 0))
    {
        result = WEB_FILE_OPERATION_IO_ERROR;
    }
    if (fclose(part) != 0 && result == WEB_FILE_OPERATION_OK)
    {
        result = WEB_FILE_OPERATION_IO_ERROR;
    }

    struct stat part_info;
    if (result == WEB_FILE_OPERATION_OK
        && (stat(s_files_context.transaction_part, &part_info) != 0 || !S_ISREG(part_info.st_mode)
            || part_info.st_size < 0
            || (uint64_t) part_info.st_size != expected_length))
    {
        result = WEB_FILE_OPERATION_IO_ERROR;
    }
    return result;
}

/**
 * @brief 发送上传成功 JSON
 *
 * @param[in] request HTTP 请求
 * @param[in] overwritten 是否替换既有文件
 * @return HTTPD 发送结果
 */
static esp_err_t web_file_send_upload_success(httpd_req_t *request, bool overwritten)
{
    const esp_err_t error = web_file_set_json_response(request, overwritten ? "200 OK" : "201 Created");
    if (error != ESP_OK)
    {
        return error;
    }
    return httpd_resp_send(request,
                           overwritten ? "{\"ok\":true,\"overwritten\":true}" : "{\"ok\":true,\"overwritten\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief 验证目录项名称仅包含最短形式 UTF-8 标量且不含控制字符
 *
 * @param[in] input 目录项名称字节
 * @param[in] input_size 不含 NUL 的字节数
 * @return true 名称有效；false 名称不可安全暴露
 */
/**
 * @brief 把共享鉴权守卫结果映射为固定 HTTP 错误
 *
 * @param[in] request HTTP 请求
 * @param[in] result 守卫结果
 * @return HTTPD 发送结果
 */
esp_err_t web_file_send_guard_error(httpd_req_t *request, web_file_guard_result_t result)
{
    switch (result)
    {
        case WEB_FILE_GUARD_UNAUTHORIZED:
            return web_file_send_json_error(request,
                                            "401 Unauthorized",
                                            "{\"error\":\"unauthorized\",\"message\":\"认证信息无效或已过期\"}");
        case WEB_FILE_GUARD_BUSY:
            return web_file_send_json_error(request,
                                            "409 Conflict",
                                            "{\"error\":\"busy\",\"message\":\"另一个文件请求正在进行\"}");
        case WEB_FILE_GUARD_UNAVAILABLE:
            return web_file_send_json_error(request,
                                            "503 Service Unavailable",
                                            "{\"error\":\"service_unavailable\",\"message\":\"服务正在停止\"}");
        case WEB_FILE_GUARD_OK:
        default:
            return ESP_FAIL;
    }
}

/**
 * @brief 把传输预检错误映射为固定 HTTP JSON
 *
 * @param[in] request HTTP 请求
 * @param[in] result 操作结果
 * @return HTTPD 发送结果；取消或已开始响应的错误由调用方直接中止连接
 */
esp_err_t web_file_send_operation_error(httpd_req_t *request, web_file_operation_result_t result)
{
    switch (result)
    {
        case WEB_FILE_OPERATION_BAD_REQUEST:
            return web_file_send_json_error(request,
                                            "400 Bad Request",
                                            "{\"error\":\"invalid_request\",\"message\":\"查询参数或路径无效\"}");
        case WEB_FILE_OPERATION_NOT_FOUND:
            return web_file_send_json_error(request,
                                            "404 Not Found",
                                            "{\"error\":\"not_found\",\"message\":\"文件或目录不存在\"}");
        case WEB_FILE_OPERATION_WRONG_TYPE:
            return web_file_send_json_error(request,
                                            "409 Conflict",
                                            "{\"error\":\"wrong_type\",\"message\":\"请求路径类型不匹配\"}");
        case WEB_FILE_OPERATION_LENGTH_REQUIRED:
            return web_file_send_json_error(
                request,
                "411 Length Required",
                "{\"error\":\"length_required\",\"message\":\"上传必须提供有效的 Content-Length\"}");
        case WEB_FILE_OPERATION_TOO_LARGE:
            return web_file_send_json_error(
                request,
                "413 Content Too Large",
                "{\"error\":\"content_too_large\",\"message\":\"上传正文超过配置上限\"}");
        case WEB_FILE_OPERATION_OVERWRITE_REQUIRED:
            return web_file_send_json_error(
                request,
                "409 Conflict",
                "{\"error\":\"overwrite_required\",\"message\":\"同名文件需要明确确认覆盖\"}");
        case WEB_FILE_OPERATION_ALREADY_EXISTS:
            return web_file_send_json_error(request,
                                            "409 Conflict",
                                            "{\"error\":\"already_exists\",\"message\":\"目标文件或目录已存在\"}");
        case WEB_FILE_OPERATION_DIRECTORY_NOT_EMPTY:
            return web_file_send_json_error(request,
                                            "409 Conflict",
                                            "{\"error\":\"directory_not_empty\",\"message\":\"目录不为空，不能删除\"}");
        case WEB_FILE_OPERATION_ROOT_FORBIDDEN:
            return web_file_send_json_error(request,
                                            "403 Forbidden",
                                            "{\"error\":\"root_forbidden\",\"message\":\"不能修改或删除根目录\"}");
        case WEB_FILE_OPERATION_INSUFFICIENT_STORAGE:
            return web_file_send_json_error(request,
                                            "507 Insufficient Storage",
                                            "{\"error\":\"insufficient_storage\",\"message\":\"存储可用空间不足\"}");
        case WEB_FILE_OPERATION_NO_MEMORY:
            return web_file_send_json_error(request,
                                            "500 Internal Server Error",
                                            "{\"error\":\"out_of_memory\",\"message\":\"传输内存分配失败\"}");
        case WEB_FILE_OPERATION_IO_ERROR:
            return web_file_send_json_error(request,
                                            "500 Internal Server Error",
                                            "{\"error\":\"filesystem\",\"message\":\"文件系统操作失败\"}");
        case WEB_FILE_OPERATION_CANCELLED:
        case WEB_FILE_OPERATION_OK:
        default:
            return ESP_FAIL;
    }
}

/**
 * @brief 执行原始 PUT 上传的完整预检、接收和事务提交
 *
 * 所有拒绝都发生在创建事务目录和接收正文之前；已发送错误响应后返回失败，使 HTTPD 关闭
 * 仍带未消费正文的连接而不是在 handler 外继续清空大文件。事务提交开始后不执行普通临时
 * 文件删除，交由事务内核按 durable 阶段恢复。
 *
 * @param[in] request HTTP 请求
 * @return ESP_OK 成功响应已发送；其他错误表示拒绝、取消或连接应关闭
 */
esp_err_t web_file_handle_file_put(httpd_req_t *request)
{
    if (request == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const web_file_guard_result_t guard_result = web_file_transfer_acquire(request);
    if (guard_result != WEB_FILE_GUARD_OK)
    {
        (void) web_file_send_guard_error(request, guard_result);
        return ESP_FAIL;
    }

    web_file_transfer_workspace_t *workspace = static_cast<web_file_transfer_workspace_t *>(
        heap_caps_malloc(sizeof(*workspace), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    web_file_upload_request_t   upload{};
    web_file_operation_result_t result = workspace == NULL
                                             ? WEB_FILE_OPERATION_NO_MEMORY
                                             : web_file_upload_read_length(request, &upload.expected_length);
    if (result == WEB_FILE_OPERATION_OK)
    {
        result = web_file_upload_read_overwrite(request, &upload.overwrite_confirmed);
    }
    if (result == WEB_FILE_OPERATION_OK)
    {
        result = web_file_read_and_map_path(request,
                                            workspace->logical,
                                            sizeof(workspace->logical),
                                            workspace->filesystem,
                                            sizeof(workspace->filesystem));
    }
    if (result == WEB_FILE_OPERATION_OK)
    {
        result = web_file_upload_validate_target(workspace, upload.overwrite_confirmed, &upload.target_exists);
    }
    if (result == WEB_FILE_OPERATION_OK)
    {
        result = web_file_upload_validate_space(upload.expected_length);
    }

    bool transaction_prepared = false;
    if (result == WEB_FILE_OPERATION_OK)
    {
        if (web_file_transaction_prepare_upload() != ESP_OK)
        {
            result = WEB_FILE_OPERATION_IO_ERROR;
        }
        else
        {
            transaction_prepared = true;
        }
    }

    uint8_t *buffer = NULL;
    if (result == WEB_FILE_OPERATION_OK)
    {
        buffer = static_cast<uint8_t *>(
            heap_caps_malloc(WEB_FILE_TRANSFER_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buffer == NULL)
        {
            result = WEB_FILE_OPERATION_NO_MEMORY;
        }
        else if (!web_file_publish_transfer_buffer(buffer))
        {
            heap_caps_free(buffer);
            buffer = NULL;
            result = WEB_FILE_OPERATION_IO_ERROR;
        }
    }

    if (result == WEB_FILE_OPERATION_OK)
    {
        result = web_file_upload_receive_part(request, buffer, upload.expected_length);
    }
    if (result != WEB_FILE_OPERATION_OK && transaction_prepared)
    {
        const esp_err_t cleanup_error = web_file_transaction_abort_upload();
        if (cleanup_error != ESP_OK && result != WEB_FILE_OPERATION_CANCELLED)
        {
            result = WEB_FILE_OPERATION_IO_ERROR;
        }
    }

    bool response_started = false;
    if (result == WEB_FILE_OPERATION_OK)
    {
        web_file_transaction_t transaction{};
        transaction.phase           = WEB_FILE_TRANSACTION_PREPARED;
        transaction.expected_length = upload.expected_length;
        memcpy(transaction.target_path, workspace->logical, strlen(workspace->logical) + 1U);

        const esp_err_t commit_error = upload.target_exists ? web_file_transaction_commit(&transaction)
                                                            : web_file_transaction_commit_new(&transaction);
        if (commit_error != ESP_OK)
        {
            /*
             * 提交内核若尚未创建 journal，这里会删除普通 `.part`；已经进入 durable 阶段时
             * abort 会拒绝改动，保留提交内核的可恢复现场。
             */
            (void) web_file_transaction_abort_upload();
            result = commit_error == ESP_ERR_INVALID_STATE && web_file_transfer_is_cancelled()
                         ? WEB_FILE_OPERATION_CANCELLED
                         : WEB_FILE_OPERATION_IO_ERROR;
        }
    }

    esp_err_t error = ESP_OK;
    if (result == WEB_FILE_OPERATION_OK)
    {
        if (web_file_transfer_is_cancelled())
        {
            result = WEB_FILE_OPERATION_CANCELLED;
            error  = ESP_FAIL;
        }
        else
        {
            response_started = true;
            error            = web_file_send_upload_success(request, upload.target_exists);
        }
    }
    if (result != WEB_FILE_OPERATION_OK)
    {
        if (result == WEB_FILE_OPERATION_CANCELLED || response_started)
        {
            error = ESP_FAIL;
        }
        else
        {
            (void) web_file_send_operation_error(request, result);
            error = ESP_FAIL;
        }
    }

    if (workspace != NULL)
    {
        heap_caps_free(workspace);
    }
    web_file_transfer_release();
    return error;
}
