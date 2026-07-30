/**
 * @file web_console_service_read.cpp
 * @brief 网页文件服务的目录浏览与常规文件下载实现
 */
#include "web_console_service_transfer.hpp"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "system_filesystem.h"

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

    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(WEB_FILE_TRANSFER_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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

    web_file_transfer_workspace_t *workspace = static_cast<web_file_transfer_workspace_t *>(
        heap_caps_malloc(sizeof(*workspace), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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


esp_err_t web_file_handle_files_get(httpd_req_t *request)
{
    return web_file_handle_read_request(request, true);
}

esp_err_t web_file_handle_file_get(httpd_req_t *request)
{
    return web_file_handle_read_request(request, false);
}
