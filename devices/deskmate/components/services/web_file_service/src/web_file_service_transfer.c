/**
 * @file web_file_service_transfer.c
 * @brief 网页文件服务的目录浏览、下载与上传传输实现
 */
#include "web_file_service_internal.h"

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
#include "system_filesystem.h"

#define WEB_FILE_AUTHORIZATION_BUFFER_SIZE 48U
#define WEB_FILE_RESPONSE_SCRATCH_SIZE     1280U
#define WEB_FILE_PATH_SEGMENT_MAX_BYTES    255U
#define WEB_FILE_UPLOAD_SPACE_MARGIN_BYTES (1U * 1024U * 1024U)
#define WEB_FILE_UPLOAD_TIMEOUT_RETRIES    1U

typedef enum
{
    WEB_FILE_GUARD_OK = 0,
    WEB_FILE_GUARD_UNAUTHORIZED,
    WEB_FILE_GUARD_BUSY,
    WEB_FILE_GUARD_UNAVAILABLE,
} web_file_guard_result_t;

typedef enum
{
    WEB_FILE_OPERATION_OK = 0,
    WEB_FILE_OPERATION_BAD_REQUEST,
    WEB_FILE_OPERATION_NOT_FOUND,
    WEB_FILE_OPERATION_WRONG_TYPE,
    WEB_FILE_OPERATION_LENGTH_REQUIRED,
    WEB_FILE_OPERATION_TOO_LARGE,
    WEB_FILE_OPERATION_OVERWRITE_REQUIRED,
    WEB_FILE_OPERATION_ALREADY_EXISTS,
    WEB_FILE_OPERATION_DIRECTORY_NOT_EMPTY,
    WEB_FILE_OPERATION_ROOT_FORBIDDEN,
    WEB_FILE_OPERATION_INSUFFICIENT_STORAGE,
    WEB_FILE_OPERATION_NO_MEMORY,
    WEB_FILE_OPERATION_IO_ERROR,
    WEB_FILE_OPERATION_CANCELLED,
} web_file_operation_result_t;

typedef struct
{
    size_t expected_length;
    bool   overwrite_confirmed;
    bool   target_exists;
} web_file_upload_request_t;

typedef struct
{
    char logical[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];
    char filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
    char scratch[WEB_FILE_RESPONSE_SCRATCH_SIZE];
    char auxiliary[WEB_FILE_RESPONSE_SCRATCH_SIZE];
} web_file_transfer_workspace_t;

typedef struct
{
    char logical[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];
    char filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
} web_file_path_workspace_t;

typedef struct
{
    web_file_path_workspace_t source;
    web_file_path_workspace_t destination;
} web_file_move_workspace_t;

typedef enum
{
    WEB_FILE_MUTATION_CREATE_DIRECTORY = 0,
    WEB_FILE_MUTATION_MOVE_FILE,
    WEB_FILE_MUTATION_DELETE_ITEM,
} web_file_mutation_t;

typedef struct
{
    const char *extension;
    const char *content_type;
} web_file_mime_entry_t;

/**
 * @brief 覆盖栈上的认证头副本
 *
 * @param[out] data 待覆盖内存
 * @param[in] size 内存长度
 */
static void web_file_secure_clear(void *data, size_t size)
{
    volatile uint8_t *cursor = (volatile uint8_t *) data;
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
static esp_err_t web_file_set_json_response(httpd_req_t *request, const char *status)
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
static web_file_guard_result_t web_file_transfer_acquire(httpd_req_t *request)
{
    char         authorization[WEB_FILE_AUTHORIZATION_BUFFER_SIZE] = { 0 };
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
    if (!s_context.accepting_requests || s_context.state != WEB_FILE_SERVICE_STATE_RUNNING)
    {
        result = WEB_FILE_GUARD_UNAVAILABLE;
    }
    else if (web_file_auth_authorize(&s_context.auth,
                                     authorization + sizeof("Bearer ") - 1U,
                                     esp_timer_get_time(),
                                     s_context.transfer_active)
             != WEB_FILE_AUTH_OK)
    {
        result = WEB_FILE_GUARD_UNAUTHORIZED;
    }
    else if (s_context.transfer_active)
    {
        result = WEB_FILE_GUARD_BUSY;
    }
    else
    {
        s_context.transfer_active        = true;
        s_context.active_transfer_socket = httpd_req_to_sockfd(request);
        result                           = WEB_FILE_GUARD_OK;
    }
    xSemaphoreGive(s_context.lock);

    web_file_secure_clear(authorization, sizeof(authorization));
    return result;
}

/**
 * @brief 释放当前请求持有的传输状态与可选 PSRAM 缓冲区
 */
static void web_file_transfer_release(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    uint8_t *buffer                  = s_context.transfer_buffer;
    s_context.transfer_buffer        = NULL;
    s_context.transfer_active        = false;
    s_context.active_transfer_socket = -1;
    web_file_auth_touch_active_session(&s_context.auth, esp_timer_get_time());
    xSemaphoreGive(s_context.lock);

    if (buffer != NULL)
    {
        heap_caps_free(buffer);
    }
}

/**
 * @brief 检查停止流程是否已经取消当前传输
 *
 * @return true Service 已停止接纳请求；false 传输可继续
 */
static bool web_file_transfer_is_cancelled(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    const bool cancelled = !s_context.accepting_requests || s_context.state != WEB_FILE_SERVICE_STATE_RUNNING;
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
static web_file_operation_result_t web_file_read_and_map_path(httpd_req_t *request, char *logical, size_t logical_size,
                                                              char *filesystem, size_t filesystem_size)
{
    const size_t query_size = httpd_req_get_url_query_len(request);
    if (query_size <= sizeof("path=") - 1U || query_size > CONFIG_HTTPD_MAX_URI_LEN)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }

    char *query = heap_caps_malloc(query_size + 1U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
static web_file_operation_result_t web_file_read_and_map_move_paths(httpd_req_t               *request,
                                                                    web_file_move_workspace_t *workspace)
{
    static const char prefix[]    = "path=";
    static const char separator[] = "&destination=";

    const size_t query_size       = httpd_req_get_url_query_len(request);
    if (query_size <= sizeof(prefix) - 1U + sizeof(separator) - 1U || query_size > CONFIG_HTTPD_MAX_URI_LEN)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }

    char *query = heap_caps_malloc(query_size + 1U, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
    if (parsed > WEB_FILE_UPLOAD_MAX_SIZE_BYTES)
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
    const size_t header_size = httpd_req_get_hdr_value_len(request, "X-DeskMate-Overwrite");
    if (header_size == 0U)
    {
        *out_confirmed = false;
        return WEB_FILE_OPERATION_OK;
    }

    char header[sizeof("confirm")];
    if (header_size != sizeof("confirm") - 1U
        || httpd_req_get_hdr_value_str(request, "X-DeskMate-Overwrite", header, sizeof(header)) != ESP_OK
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
 * @brief 要求文件系统剩余空间覆盖完整暂存文件及固定一 MiB 余量
 *
 * ESP-IDF v6.0.1 的 `statvfs()` 是固定返回 ENOSYS 的占位实现，因此使用 System 层公开的
 * `system_filesystem_get_info_copy()`；其底层通过 FatFs 容量查询返回同一挂载点可用字节。
 *
 * @param[in] expected_length 上传正文长度
 * @return 操作结果
 */
static web_file_operation_result_t web_file_upload_validate_space(size_t expected_length)
{
    system_filesystem_info_t filesystem_info;
    if (system_filesystem_get_info_copy(&filesystem_info) != ESP_OK)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    const uint64_t required = (uint64_t) expected_length + WEB_FILE_UPLOAD_SPACE_MARGIN_BYTES;
    return filesystem_info.free_bytes >= required ? WEB_FILE_OPERATION_OK : WEB_FILE_OPERATION_INSUFFICIENT_STORAGE;
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
    const int descriptor = open(WEB_FILE_TRANSACTION_PART, O_WRONLY | O_CREAT | O_EXCL, 0600);
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
        && (stat(WEB_FILE_TRANSACTION_PART, &part_info) != 0 || !S_ISREG(part_info.st_mode) || part_info.st_size < 0
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
static bool web_file_entry_utf8_is_valid(const uint8_t *input, size_t input_size)
{
    size_t offset = 0U;
    while (offset < input_size)
    {
        const uint8_t first = input[offset];
        uint32_t      scalar;
        size_t        sequence_size;

        if (first <= 0x7FU)
        {
            scalar        = first;
            sequence_size = 1U;
        }
        else if (first >= 0xC2U && first <= 0xDFU)
        {
            if (offset + 1U >= input_size || (input[offset + 1U] & 0xC0U) != 0x80U)
            {
                return false;
            }
            scalar        = ((uint32_t) (first & 0x1FU) << 6U) | (uint32_t) (input[offset + 1U] & 0x3FU);
            sequence_size = 2U;
        }
        else if (first >= 0xE0U && first <= 0xEFU)
        {
            if (offset + 2U >= input_size || (input[offset + 1U] & 0xC0U) != 0x80U
                || (input[offset + 2U] & 0xC0U) != 0x80U)
            {
                return false;
            }
            const uint8_t second = input[offset + 1U];
            if ((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second > 0x9FU))
            {
                return false;
            }
            scalar        = ((uint32_t) (first & 0x0FU) << 12U) | ((uint32_t) (second & 0x3FU) << 6U)
                            | (uint32_t) (input[offset + 2U] & 0x3FU);
            sequence_size = 3U;
        }
        else if (first >= 0xF0U && first <= 0xF4U)
        {
            if (offset + 3U >= input_size || (input[offset + 1U] & 0xC0U) != 0x80U
                || (input[offset + 2U] & 0xC0U) != 0x80U || (input[offset + 3U] & 0xC0U) != 0x80U)
            {
                return false;
            }
            const uint8_t second = input[offset + 1U];
            if ((first == 0xF0U && second < 0x90U) || (first == 0xF4U && second > 0x8FU))
            {
                return false;
            }
            scalar        = ((uint32_t) (first & 0x07U) << 18U) | ((uint32_t) (second & 0x3FU) << 12U)
                            | ((uint32_t) (input[offset + 2U] & 0x3FU) << 6U) | (uint32_t) (input[offset + 3U] & 0x3FU);
            sequence_size = 4U;
        }
        else
        {
            return false;
        }

        if (scalar <= 0x1FU || (scalar >= 0x7FU && scalar <= 0x9FU) || (scalar >= 0xD800U && scalar <= 0xDFFFU)
            || scalar > 0x10FFFFU)
        {
            return false;
        }
        offset += sequence_size;
    }
    return true;
}

/**
 * @brief 按 ASCII 大小写不敏感语义比较两个固定字符串
 *
 * @param[in] left 左侧字符串
 * @param[in] right 右侧字符串
 * @return true 相等；false 不相等
 */
static bool web_file_ascii_case_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0')
    {
        unsigned char left_value  = (unsigned char) *left++;
        unsigned char right_value = (unsigned char) *right++;
        if (left_value >= 'A' && left_value <= 'Z')
        {
            left_value = (unsigned char) (left_value + ('a' - 'A'));
        }
        if (right_value >= 'A' && right_value <= 'Z')
        {
            right_value = (unsigned char) (right_value + ('a' - 'A'));
        }
        if (left_value != right_value)
        {
            return false;
        }
    }
    return *left == '\0' && *right == '\0';
}

/**
 * @brief 判断目录项是否属于不应返回的虚拟或保留名称
 *
 * @param[in] logical_path 当前逻辑目录
 * @param[in] name 目录项名称
 * @return true 应跳过；false 应验证并输出
 */
static bool web_file_directory_entry_is_hidden(const char *logical_path, const char *name)
{
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    {
        return true;
    }
    return strcmp(logical_path, "/") == 0 && web_file_ascii_case_equal(name, ".deskmate-web");
}

/**
 * @brief 校验并格式化单个目录项 JSON
 *
 * 调用方传入的辅助缓冲区先用于拼接完整文件系统路径，完成 stat 后改写为单项 JSON。
 *
 * @param[in] workspace 当前传输工作区
 * @param[in] name 目录项名称
 * @return 操作结果
 */
static web_file_operation_result_t web_file_format_directory_entry(web_file_transfer_workspace_t *workspace,
                                                                   const char                    *name)
{
    const size_t name_size = strnlen(name, WEB_FILE_PATH_SEGMENT_MAX_BYTES + 1U);
    if (name_size == 0U || name_size > WEB_FILE_PATH_SEGMENT_MAX_BYTES || strchr(name, '/') != NULL
        || strchr(name, '\\') != NULL || !web_file_entry_utf8_is_valid((const uint8_t *) name, name_size))
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    const int path_size =
        snprintf(workspace->auxiliary, sizeof(workspace->auxiliary), "%s/%s", workspace->filesystem, name);
    if (path_size < 0 || (size_t) path_size >= sizeof(workspace->auxiliary))
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    struct stat entry_info;
    if (stat(workspace->auxiliary, &entry_info) != 0)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    if ((!S_ISREG(entry_info.st_mode) && !S_ISDIR(entry_info.st_mode))
        || (S_ISREG(entry_info.st_mode) && entry_info.st_size < 0))
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    if (web_file_json_escape(name, workspace->scratch, sizeof(workspace->scratch)) != ESP_OK)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    const bool     is_file    = S_ISREG(entry_info.st_mode);
    const uint64_t size_bytes = is_file ? (uint64_t) entry_info.st_size : 0U;
    const int      json_size  = snprintf(workspace->auxiliary,
                                         sizeof(workspace->auxiliary),
                                         "{\"name\":\"%s\",\"type\":\"%s\",\"sizeBytes\":%" PRIu64 "}",
                                         workspace->scratch,
                                         is_file ? "file" : "directory",
                                         size_bytes);
    return json_size >= 0 && (size_t) json_size < sizeof(workspace->auxiliary) ? WEB_FILE_OPERATION_OK
                                                                               : WEB_FILE_OPERATION_IO_ERROR;
}

/**
 * @brief 关闭目录并把关闭失败并入当前结果
 *
 * @param[in] directory 已打开目录
 * @param[in] result 关闭前结果
 * @return 原结果，或在原结果成功时返回 I/O 错误
 */
static web_file_operation_result_t web_file_close_directory(DIR *directory, web_file_operation_result_t result)
{
    if (closedir(directory) != 0 && result == WEB_FILE_OPERATION_OK)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    return result;
}

/**
 * @brief 完整遍历目录并验证所有可见项
 *
 * @param[in] directory 已打开目录
 * @param[in,out] workspace 传输工作区
 * @return 操作结果，目录始终由本函数关闭
 */
static web_file_operation_result_t web_file_validate_directory(DIR *directory, web_file_transfer_workspace_t *workspace)
{
    web_file_operation_result_t result = WEB_FILE_OPERATION_OK;
    while (result == WEB_FILE_OPERATION_OK)
    {
        if (web_file_transfer_is_cancelled())
        {
            result = WEB_FILE_OPERATION_CANCELLED;
            break;
        }

        errno                = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                result = WEB_FILE_OPERATION_IO_ERROR;
            }
            break;
        }
        if (!web_file_directory_entry_is_hidden(workspace->logical, entry->d_name))
        {
            result = web_file_format_directory_entry(workspace, entry->d_name);
        }
    }
    return web_file_close_directory(directory, result);
}

/**
 * @brief 第二次遍历目录并逐项发送 JSON
 *
 * @param[in] request HTTP 请求
 * @param[in] directory 已重新打开的目录
 * @param[in,out] workspace 传输工作区
 * @return 操作结果，目录始终由本函数关闭
 */
static web_file_operation_result_t web_file_stream_directory_entries(httpd_req_t *request, DIR *directory,
                                                                     web_file_transfer_workspace_t *workspace)
{
    web_file_operation_result_t result = WEB_FILE_OPERATION_OK;
    bool                        first  = true;
    while (result == WEB_FILE_OPERATION_OK)
    {
        if (web_file_transfer_is_cancelled())
        {
            result = WEB_FILE_OPERATION_CANCELLED;
            break;
        }

        errno                = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                result = WEB_FILE_OPERATION_IO_ERROR;
            }
            break;
        }
        if (web_file_directory_entry_is_hidden(workspace->logical, entry->d_name))
        {
            continue;
        }

        result = web_file_format_directory_entry(workspace, entry->d_name);
        if (result != WEB_FILE_OPERATION_OK)
        {
            break;
        }
        if ((!first && httpd_resp_send_chunk(request, ",", 1U) != ESP_OK)
            || httpd_resp_send_chunk(request, workspace->auxiliary, HTTPD_RESP_USE_STRLEN) != ESP_OK)
        {
            result = WEB_FILE_OPERATION_IO_ERROR;
            break;
        }
        first = false;
    }
    return web_file_close_directory(directory, result);
}

/**
 * @brief 双遍历目录并流式发送完整 JSON
 *
 * @param[in] request HTTP 请求
 * @param[in,out] workspace 已映射路径的工作区
 * @param[out] response_started 是否已经开始构造成功响应
 * @return 操作结果
 */
static web_file_operation_result_t web_file_send_directory_listing(httpd_req_t                   *request,
                                                                   web_file_transfer_workspace_t *workspace,
                                                                   bool                          *response_started)
{
    struct stat directory_info;
    if (stat(workspace->filesystem, &directory_info) != 0)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    if (!S_ISDIR(directory_info.st_mode))
    {
        return WEB_FILE_OPERATION_WRONG_TYPE;
    }

    DIR *directory = opendir(workspace->filesystem);
    if (directory == NULL)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    web_file_operation_result_t result = web_file_validate_directory(directory, workspace);
    if (result != WEB_FILE_OPERATION_OK)
    {
        return result;
    }

    system_filesystem_info_t filesystem_info;
    if (system_filesystem_get_info_copy(&filesystem_info) != ESP_OK
        || web_file_json_escape(workspace->logical, workspace->scratch, sizeof(workspace->scratch)) != ESP_OK)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    const int prefix_size =
        snprintf(workspace->auxiliary,
                 sizeof(workspace->auxiliary),
                 "{\"path\":\"%s\",\"totalBytes\":%" PRIu64 ",\"freeBytes\":%" PRIu64 ",\"entries\":[",
                 workspace->scratch,
                 filesystem_info.total_bytes,
                 filesystem_info.free_bytes);
    if (prefix_size < 0 || (size_t) prefix_size >= sizeof(workspace->auxiliary))
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    directory = opendir(workspace->filesystem);
    if (directory == NULL)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    if (web_file_transfer_is_cancelled())
    {
        return web_file_close_directory(directory, WEB_FILE_OPERATION_CANCELLED);
    }
    *response_started = true;
    if (web_file_set_json_response(request, "200 OK") != ESP_OK
        || httpd_resp_send_chunk(request, workspace->auxiliary, (size_t) prefix_size) != ESP_OK)
    {
        return web_file_close_directory(directory, WEB_FILE_OPERATION_IO_ERROR);
    }

    result = web_file_stream_directory_entries(request, directory, workspace);
    if (result != WEB_FILE_OPERATION_OK)
    {
        return result;
    }
    if (httpd_resp_send_chunk(request, "]}", 2U) != ESP_OK || httpd_resp_send_chunk(request, NULL, 0U) != ESP_OK)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    return WEB_FILE_OPERATION_OK;
}

/**
 * @brief 根据文件名扩展名选择有界 MIME 类型
 *
 * @param[in] filename 不含目录的文件名
 * @return 静态 MIME 字符串
 */
static const char *web_file_content_type_for_name(const char *filename)
{
    static const web_file_mime_entry_t mime_table[] = {
        { "txt",  "text/plain; charset=utf-8"       },
        { "html", "text/html; charset=utf-8"        },
        { "json", "application/json; charset=utf-8" },
        { "pdf",  "application/pdf"                 },
        { "png",  "image/png"                       },
        { "jpg",  "image/jpeg"                      },
        { "jpeg", "image/jpeg"                      },
        { "gif",  "image/gif"                       },
        { "webp", "image/webp"                      },
    };

    const char *extension = strrchr(filename, '.');
    if (extension == NULL || extension[1] == '\0')
    {
        return "application/octet-stream";
    }
    ++extension;
    for (size_t index = 0U; index < sizeof(mime_table) / sizeof(mime_table[0]); ++index)
    {
        if (web_file_ascii_case_equal(extension, mime_table[index].extension))
        {
            return mime_table[index].content_type;
        }
    }
    return "application/octet-stream";
}

/**
 * @brief 把下载缓冲区发布到 Service 传输所有权
 *
 * 停止流程只会在 handler 排空后摘除该指针，因此成功发布后由统一传输清理函数释放。
 *
 * @param[in] buffer 32 KiB PSRAM 缓冲区
 * @return true 已发布；false 传输状态不一致
 */
static bool web_file_publish_transfer_buffer(uint8_t *buffer)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    const bool publish = s_context.transfer_active && s_context.transfer_buffer == NULL;
    if (publish)
    {
        s_context.transfer_buffer = buffer;
    }
    xSemaphoreGive(s_context.lock);
    return publish;
}

/**
 * @brief 使用公开低层发送 API 完整发送一段固定长度数据
 *
 * `httpd_send()` 允许部分发送，本函数持续推进游标，且在每次调用前复查停止取消。返回零、
 * 负值或超过剩余长度都视为连接失败，避免发送循环停滞或越界记账。
 *
 * @param[in] request HTTP 请求
 * @param[in] data 待发送数据
 * @param[in] data_size 数据长度
 * @return 操作结果
 */
static web_file_operation_result_t web_file_httpd_send_all(httpd_req_t *request, const char *data, size_t data_size)
{
    size_t sent_size = 0U;
    while (sent_size < data_size)
    {
        if (web_file_transfer_is_cancelled())
        {
            return WEB_FILE_OPERATION_CANCELLED;
        }

        const size_t remaining = data_size - sent_size;
        const int    sent      = httpd_send(request, data + sent_size, remaining);
        if (sent <= 0 || (size_t) sent > remaining)
        {
            return WEB_FILE_OPERATION_IO_ERROR;
        }
        sent_size += (size_t) sent;
    }
    return WEB_FILE_OPERATION_OK;
}

/**
 * @brief 构造带精确 Content-Length 的完整下载响应头
 *
 * 文件名只进入 UTF-8 `filename*` 的百分号编码值，MIME 来自静态有界表，因此响应头不存在
 * CR/LF 注入来源。输出包含头部终止空行，可直接交给 `httpd_send()`。
 *
 * @param[in,out] workspace 传输工作区
 * @param[in] filename 不含目录的文件名
 * @param[in] file_size 已打开文件的稳定长度
 * @param[out] out_header_size 完整响应头字节数
 * @return 操作结果
 */
static web_file_operation_result_t web_file_build_download_header(web_file_transfer_workspace_t *workspace,
                                                                  const char *filename, uint64_t file_size,
                                                                  size_t *out_header_size)
{
    if (web_file_percent_encode(filename, workspace->scratch, sizeof(workspace->scratch)) != ESP_OK)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    const int header_size = snprintf(workspace->auxiliary,
                                     sizeof(workspace->auxiliary),
                                     "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: %s\r\n"
                                     "Content-Length: %" PRIu64
                                     "\r\n"
                                     "Content-Disposition: attachment; filename=\"download\"; "
                                     "filename*=UTF-8''%s\r\n"
                                     "Cache-Control: no-store\r\n"
                                     "X-Content-Type-Options: nosniff\r\n"
                                     "\r\n",
                                     web_file_content_type_for_name(filename),
                                     file_size,
                                     workspace->scratch);
    if (header_size < 0 || (size_t) header_size >= sizeof(workspace->auxiliary))
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    *out_header_size = (size_t) header_size;
    return WEB_FILE_OPERATION_OK;
}

/**
 * @brief 使用 Service 的 32 KiB PSRAM 缓冲区流式发送常规文件
 *
 * 打开后用 `fstat()` 再次确认常规文件及长度未变化，再构造完整固定长度 HTTP/1.1 响应头。
 * 响应头和每个 32 KiB 原始正文块都通过 `httpd_send()` 的 send-all 循环发送，不使用会
 * 自动启用 chunked framing 的 response chunk API。任何短读、断连、发送或关闭失败都中止。
 *
 * @param[in] request HTTP 请求
 * @param[in,out] workspace 已映射路径的工作区
 * @param[out] response_started 是否已经开始构造成功响应
 * @return 操作结果
 */
static web_file_operation_result_t
    web_file_send_download(httpd_req_t *request, web_file_transfer_workspace_t *workspace, bool *response_started)
{
    struct stat file_info;
    if (stat(workspace->filesystem, &file_info) != 0)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    if (!S_ISREG(file_info.st_mode))
    {
        return WEB_FILE_OPERATION_WRONG_TYPE;
    }
    if (file_info.st_size < 0)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    FILE *file = fopen(workspace->filesystem, "rb");
    if (file == NULL)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }

    struct stat opened_file_info;
    const int   file_descriptor = fileno(file);
    if (file_descriptor < 0 || fstat(file_descriptor, &opened_file_info) != 0)
    {
        (void) fclose(file);
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    if (!S_ISREG(opened_file_info.st_mode))
    {
        (void) fclose(file);
        return WEB_FILE_OPERATION_WRONG_TYPE;
    }
    if (opened_file_info.st_size < 0 || opened_file_info.st_size != file_info.st_size)
    {
        (void) fclose(file);
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    const uint64_t file_size = (uint64_t) opened_file_info.st_size;

    uint8_t *buffer          = heap_caps_malloc(WEB_FILE_TRANSFER_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL)
    {
        (void) fclose(file);
        return WEB_FILE_OPERATION_NO_MEMORY;
    }
    if (!web_file_publish_transfer_buffer(buffer))
    {
        heap_caps_free(buffer);
        (void) fclose(file);
        return WEB_FILE_OPERATION_IO_ERROR;
    }

    const char *filename = strrchr(workspace->logical, '/');
    filename             = filename == NULL ? workspace->logical : filename + 1U;
    size_t header_size   = 0U;
    if (filename[0] == '\0'
        || web_file_build_download_header(workspace, filename, file_size, &header_size) != WEB_FILE_OPERATION_OK)
    {
        (void) fclose(file);
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    if (web_file_transfer_is_cancelled())
    {
        (void) fclose(file);
        return WEB_FILE_OPERATION_CANCELLED;
    }

    *response_started                  = true;
    web_file_operation_result_t result = web_file_httpd_send_all(request, workspace->auxiliary, header_size);

    uint64_t sent_size                 = 0U;
    while (result == WEB_FILE_OPERATION_OK && sent_size < file_size)
    {
        if (web_file_transfer_is_cancelled())
        {
            result = WEB_FILE_OPERATION_CANCELLED;
            break;
        }

        const uint64_t remaining = file_size - sent_size;
        const size_t   read_size =
            remaining > WEB_FILE_TRANSFER_BUFFER_SIZE ? WEB_FILE_TRANSFER_BUFFER_SIZE : (size_t) remaining;
        const size_t actual_size = fread(buffer, 1U, read_size, file);
        if (actual_size != read_size)
        {
            result = WEB_FILE_OPERATION_IO_ERROR;
            break;
        }
        result = web_file_httpd_send_all(request, (const char *) buffer, actual_size);
        if (result != WEB_FILE_OPERATION_OK)
        {
            break;
        }
        sent_size += actual_size;
    }
    if (result == WEB_FILE_OPERATION_OK && ferror(file) != 0)
    {
        result = WEB_FILE_OPERATION_IO_ERROR;
    }
    if (fclose(file) != 0 && result == WEB_FILE_OPERATION_OK)
    {
        result = WEB_FILE_OPERATION_IO_ERROR;
    }
    return result;
}

/**
 * @brief 把共享鉴权守卫结果映射为固定 HTTP 错误
 *
 * @param[in] request HTTP 请求
 * @param[in] result 守卫结果
 * @return HTTPD 发送结果
 */
static esp_err_t web_file_send_guard_error(httpd_req_t *request, web_file_guard_result_t result)
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
static esp_err_t web_file_send_operation_error(httpd_req_t *request, web_file_operation_result_t result)
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
                "{\"error\":\"content_too_large\",\"message\":\"上传文件不能超过 500 MiB\"}");
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
                                            "{\"error\":\"insufficient_storage\",\"message\":\"SD 卡可用空间不足\"}");
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
 * @brief 校验目标路径的父目录真实存在且为目录
 *
 * 本函数会暂时在可写路径缓冲区中截断最后一个分隔符，`stat()` 返回后恢复原路径。
 *
 * @param[in,out] filesystem 目标文件系统路径
 * @return 操作结果
 */
static web_file_operation_result_t web_file_validate_parent_directory(char *filesystem)
{
    char *separator = strrchr(filesystem, '/');
    if (separator == NULL || separator == filesystem)
    {
        return WEB_FILE_OPERATION_BAD_REQUEST;
    }

    const char saved = *separator;
    *separator       = '\0';
    struct stat parent_info;
    const int   stat_result = stat(filesystem, &parent_info);
    const int   stat_errno  = errno;
    *separator              = saved;

    if (stat_result != 0)
    {
        return stat_errno == ENOENT || stat_errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND
                                                             : WEB_FILE_OPERATION_IO_ERROR;
    }
    return S_ISDIR(parent_info.st_mode) ? WEB_FILE_OPERATION_OK : WEB_FILE_OPERATION_WRONG_TYPE;
}

/**
 * @brief 创建一个已通过路径内核校验的新目录
 *
 * @param[in,out] path 目标逻辑路径和文件系统路径
 * @return 操作结果
 */
static web_file_operation_result_t web_file_create_directory(web_file_path_workspace_t *path)
{
    if (strcmp(path->logical, "/") == 0)
    {
        return WEB_FILE_OPERATION_ROOT_FORBIDDEN;
    }

    web_file_operation_result_t result = web_file_validate_parent_directory(path->filesystem);
    if (result != WEB_FILE_OPERATION_OK)
    {
        return result;
    }

    struct stat target_info;
    if (stat(path->filesystem, &target_info) == 0)
    {
        return WEB_FILE_OPERATION_ALREADY_EXISTS;
    }
    if (errno != ENOENT)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    if (web_file_transfer_is_cancelled())
    {
        return WEB_FILE_OPERATION_CANCELLED;
    }
    if (mkdir(path->filesystem, 0775) == 0)
    {
        return WEB_FILE_OPERATION_OK;
    }
    return errno == EEXIST ? WEB_FILE_OPERATION_ALREADY_EXISTS : WEB_FILE_OPERATION_IO_ERROR;
}

/**
 * @brief 原子重命名或移动一个已校验的常规文件
 *
 * 源和目标都位于固定 SD 卡挂载点，目标必须不存在且父目录必须已经存在。相同源和目标按成功
 * 的无操作处理。
 *
 * @param[in,out] workspace 源和目标路径
 * @return 操作结果
 */
static web_file_operation_result_t web_file_move_file(web_file_move_workspace_t *workspace)
{
    if (strcmp(workspace->source.logical, "/") == 0 || strcmp(workspace->destination.logical, "/") == 0)
    {
        return WEB_FILE_OPERATION_ROOT_FORBIDDEN;
    }

    struct stat source_info;
    if (stat(workspace->source.filesystem, &source_info) != 0)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    if (!S_ISREG(source_info.st_mode))
    {
        return WEB_FILE_OPERATION_WRONG_TYPE;
    }
    if (strcmp(workspace->source.logical, workspace->destination.logical) == 0)
    {
        return WEB_FILE_OPERATION_OK;
    }

    web_file_operation_result_t result = web_file_validate_parent_directory(workspace->destination.filesystem);
    if (result != WEB_FILE_OPERATION_OK)
    {
        return result;
    }

    struct stat destination_info;
    if (stat(workspace->destination.filesystem, &destination_info) == 0)
    {
        return WEB_FILE_OPERATION_ALREADY_EXISTS;
    }
    if (errno != ENOENT)
    {
        return WEB_FILE_OPERATION_IO_ERROR;
    }
    if (web_file_transfer_is_cancelled())
    {
        return WEB_FILE_OPERATION_CANCELLED;
    }
    if (rename(workspace->source.filesystem, workspace->destination.filesystem) == 0)
    {
        return WEB_FILE_OPERATION_OK;
    }
    if (errno == EEXIST)
    {
        return WEB_FILE_OPERATION_ALREADY_EXISTS;
    }
    return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
}

/**
 * @brief 检查目录是否为空
 *
 * @param[in] filesystem 目录文件系统路径
 * @param[out] out_empty 是否为空
 * @return 操作结果
 */
static web_file_operation_result_t web_file_directory_is_empty(const char *filesystem, bool *out_empty)
{
    DIR *directory = opendir(filesystem);
    if (directory == NULL)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }

    *out_empty = true;
    errno      = 0;
    for (;;)
    {
        struct dirent *entry = readdir(directory);
        if (entry == NULL)
        {
            break;
        }
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
        {
            *out_empty = false;
            break;
        }
    }
    const int read_errno  = errno;
    const int close_error = closedir(directory);
    return read_errno == 0 && close_error == 0 ? WEB_FILE_OPERATION_OK : WEB_FILE_OPERATION_IO_ERROR;
}

/**
 * @brief 删除一个常规文件或空目录
 *
 * @param[in] path 目标逻辑路径和文件系统路径
 * @return 操作结果
 */
static web_file_operation_result_t web_file_delete_item(const web_file_path_workspace_t *path)
{
    if (strcmp(path->logical, "/") == 0)
    {
        return WEB_FILE_OPERATION_ROOT_FORBIDDEN;
    }

    struct stat target_info;
    if (stat(path->filesystem, &target_info) != 0)
    {
        return errno == ENOENT || errno == ENOTDIR ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }

    if (S_ISREG(target_info.st_mode))
    {
        if (web_file_transfer_is_cancelled())
        {
            return WEB_FILE_OPERATION_CANCELLED;
        }
        if (unlink(path->filesystem) == 0)
        {
            return WEB_FILE_OPERATION_OK;
        }
        return errno == ENOENT ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
    }
    if (!S_ISDIR(target_info.st_mode))
    {
        return WEB_FILE_OPERATION_WRONG_TYPE;
    }

    bool                        empty;
    web_file_operation_result_t result = web_file_directory_is_empty(path->filesystem, &empty);
    if (result != WEB_FILE_OPERATION_OK)
    {
        return result;
    }
    if (!empty)
    {
        return WEB_FILE_OPERATION_DIRECTORY_NOT_EMPTY;
    }
    if (web_file_transfer_is_cancelled())
    {
        return WEB_FILE_OPERATION_CANCELLED;
    }
    if (rmdir(path->filesystem) == 0)
    {
        return WEB_FILE_OPERATION_OK;
    }
    if (errno == ENOTEMPTY || errno == EEXIST)
    {
        return WEB_FILE_OPERATION_DIRECTORY_NOT_EMPTY;
    }
    return errno == ENOENT ? WEB_FILE_OPERATION_NOT_FOUND : WEB_FILE_OPERATION_IO_ERROR;
}

/**
 * @brief 发送文件变更成功响应
 *
 * @param[in] request HTTP 请求
 * @param[in] mutation 变更类型
 * @return HTTPD 发送结果
 */
static esp_err_t web_file_send_mutation_success(httpd_req_t *request, web_file_mutation_t mutation)
{
    const char *status = mutation == WEB_FILE_MUTATION_CREATE_DIRECTORY ? "201 Created" : "200 OK";
    const char *body;
    switch (mutation)
    {
        case WEB_FILE_MUTATION_CREATE_DIRECTORY:
            body = "{\"ok\":true,\"message\":\"目录已创建\"}";
            break;
        case WEB_FILE_MUTATION_MOVE_FILE:
            body = "{\"ok\":true,\"message\":\"文件已移动\"}";
            break;
        case WEB_FILE_MUTATION_DELETE_ITEM:
        default:
            body = "{\"ok\":true,\"message\":\"项目已删除\"}";
            break;
    }

    const esp_err_t error = web_file_set_json_response(request, status);
    return error == ESP_OK ? httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN) : error;
}

/**
 * @brief 执行共享鉴权、严格路径解析和一个短时文件变更
 *
 * 本函数复用只读和上传请求的唯一传输所有权，保证目录创建、移动和删除不会与同一 Service 的
 * 其他文件请求并发。每次请求只提交一个文件系统变更，多选操作由浏览器按顺序调用。
 *
 * @param[in] request HTTP 请求
 * @param[in] mutation 变更类型
 * @return ESP_OK 响应成功；其他错误码表示请求被拒绝或发送失败
 */
static esp_err_t web_file_handle_mutation_request(httpd_req_t *request, web_file_mutation_t mutation)
{
    if (request == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!web_file_handler_enter())
    {
        web_file_handler_leave();
        return ESP_FAIL;
    }

    const web_file_guard_result_t guard_result = web_file_transfer_acquire(request);
    if (guard_result != WEB_FILE_GUARD_OK)
    {
        const esp_err_t error = web_file_send_guard_error(request, guard_result);
        web_file_handler_leave();
        return error;
    }

    const bool move = mutation == WEB_FILE_MUTATION_MOVE_FILE;
    void *workspace = heap_caps_malloc(move ? sizeof(web_file_move_workspace_t) : sizeof(web_file_path_workspace_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    web_file_operation_result_t result = WEB_FILE_OPERATION_NO_MEMORY;
    if (workspace != NULL)
    {
        if (move)
        {
            web_file_move_workspace_t *move_workspace = workspace;
            result                                    = web_file_read_and_map_move_paths(request, move_workspace);
            if (result == WEB_FILE_OPERATION_OK)
            {
                result = web_file_move_file(move_workspace);
            }
        }
        else
        {
            web_file_path_workspace_t *path = workspace;
            result                          = web_file_read_and_map_path(request,
                                                                         path->logical,
                                                                         sizeof(path->logical),
                                                                         path->filesystem,
                                                                         sizeof(path->filesystem));
            if (result == WEB_FILE_OPERATION_OK)
            {
                result = mutation == WEB_FILE_MUTATION_CREATE_DIRECTORY ? web_file_create_directory(path)
                                                                        : web_file_delete_item(path);
            }
        }
    }

    const esp_err_t error =
        result == WEB_FILE_OPERATION_OK
            ? web_file_send_mutation_success(request, mutation)
            : (result == WEB_FILE_OPERATION_CANCELLED ? ESP_FAIL : web_file_send_operation_error(request, result));
    if (workspace != NULL)
    {
        heap_caps_free(workspace);
    }
    web_file_transfer_release();
    web_file_handler_leave();
    return error;
}

/**
 * @brief 执行共享鉴权、严格路径解析和一种只读文件事务
 *
 * @param[in] request HTTP 请求
 * @param[in] list_directory true 浏览目录；false 下载文件
 * @return ESP_OK 响应成功；其他错误码表示请求被拒绝或传输中止
 */
static esp_err_t web_file_handle_read_request(httpd_req_t *request, bool list_directory)
{
    if (request == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!web_file_handler_enter())
    {
        web_file_handler_leave();
        return ESP_FAIL;
    }

    const web_file_guard_result_t guard_result = web_file_transfer_acquire(request);
    if (guard_result != WEB_FILE_GUARD_OK)
    {
        const esp_err_t error = web_file_send_guard_error(request, guard_result);
        web_file_handler_leave();
        return error;
    }

    web_file_transfer_workspace_t *workspace =
        heap_caps_malloc(sizeof(*workspace), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    web_file_operation_result_t result = workspace == NULL ? WEB_FILE_OPERATION_NO_MEMORY
                                                           : web_file_read_and_map_path(request,
                                                                                        workspace->logical,
                                                                                        sizeof(workspace->logical),
                                                                                        workspace->filesystem,
                                                                                        sizeof(workspace->filesystem));
    bool                        response_started = false;
    if (result == WEB_FILE_OPERATION_OK)
    {
        result = list_directory ? web_file_send_directory_listing(request, workspace, &response_started)
                                : web_file_send_download(request, workspace, &response_started);
    }

    esp_err_t error = ESP_OK;
    if (result != WEB_FILE_OPERATION_OK)
    {
        error = response_started || result == WEB_FILE_OPERATION_CANCELLED
                    ? ESP_FAIL
                    : web_file_send_operation_error(request, result);
    }
    if (workspace != NULL)
    {
        heap_caps_free(workspace);
    }
    web_file_transfer_release();
    web_file_handler_leave();
    return error;
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
    if (!web_file_handler_enter())
    {
        web_file_handler_leave();
        return ESP_FAIL;
    }

    const web_file_guard_result_t guard_result = web_file_transfer_acquire(request);
    if (guard_result != WEB_FILE_GUARD_OK)
    {
        (void) web_file_send_guard_error(request, guard_result);
        web_file_handler_leave();
        return ESP_FAIL;
    }

    web_file_transfer_workspace_t *workspace =
        heap_caps_malloc(sizeof(*workspace), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    web_file_upload_request_t   upload = { 0 };
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
        buffer = heap_caps_malloc(WEB_FILE_TRANSFER_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
        web_file_transaction_t transaction = {
            .phase           = WEB_FILE_TRANSACTION_PREPARED,
            .expected_length = upload.expected_length,
        };
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
    web_file_handler_leave();
    return error;
}

esp_err_t web_file_handle_files_get(httpd_req_t *request)
{
    return web_file_handle_read_request(request, true);
}

esp_err_t web_file_handle_file_get(httpd_req_t *request)
{
    return web_file_handle_read_request(request, false);
}

esp_err_t web_file_handle_directory_put(httpd_req_t *request)
{
    return web_file_handle_mutation_request(request, WEB_FILE_MUTATION_CREATE_DIRECTORY);
}

esp_err_t web_file_handle_file_patch(httpd_req_t *request)
{
    return web_file_handle_mutation_request(request, WEB_FILE_MUTATION_MOVE_FILE);
}

esp_err_t web_file_handle_file_delete(httpd_req_t *request)
{
    return web_file_handle_mutation_request(request, WEB_FILE_MUTATION_DELETE_ITEM);
}
