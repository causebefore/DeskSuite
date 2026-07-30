/**
 * @file web_console_service_transfer.hpp
 * @brief 网页文件服务各 C++ 传输实现共享的有界类型和私有接口
 */
#pragma once

#include <cstddef>

#include "web_console_service_internal.hpp"

#define WEB_FILE_AUTHORIZATION_BUFFER_SIZE 48U
#define WEB_FILE_RESPONSE_SCRATCH_SIZE     1280U
#define WEB_FILE_PATH_SEGMENT_MAX_BYTES    255U
#define WEB_FILE_UPLOAD_SPACE_MARGIN_BYTES (1U * 1024U * 1024U)
#define WEB_FILE_UPLOAD_TIMEOUT_RETRIES    1U

enum web_file_guard_result_t
{
    WEB_FILE_GUARD_OK = 0,
    WEB_FILE_GUARD_UNAUTHORIZED,
    WEB_FILE_GUARD_BUSY,
    WEB_FILE_GUARD_UNAVAILABLE,
};

enum web_file_operation_result_t
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
};

struct web_file_upload_request_t
{
    size_t expected_length;
    bool   overwrite_confirmed;
    bool   target_exists;
};

struct web_file_transfer_workspace_t
{
    char logical[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];
    char filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
    char scratch[WEB_FILE_RESPONSE_SCRATCH_SIZE];
    char auxiliary[WEB_FILE_RESPONSE_SCRATCH_SIZE];
};

struct web_file_path_workspace_t
{
    char logical[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];
    char filesystem[WEB_FILE_FILESYSTEM_PATH_BUFFER_SIZE];
};

struct web_file_move_workspace_t
{
    web_file_path_workspace_t source;
    web_file_path_workspace_t destination;
};

enum web_file_mutation_t
{
    WEB_FILE_MUTATION_CREATE_DIRECTORY = 0,
    WEB_FILE_MUTATION_MOVE_FILE,
    WEB_FILE_MUTATION_DELETE_ITEM,
};

struct web_file_mime_entry_t
{
    const char *extension;
    const char *content_type;
};

/**
 * @brief 设置 JSON 响应状态、类型与禁止缓存响应头
 *
 * @param[in] request HTTP 请求
 * @param[in] status HTTP 状态文本
 * @return ESP_OK 设置完成；其他错误码来自 HTTPD
 */
esp_err_t web_file_set_json_response(httpd_req_t *request, const char *status);

/** @brief 鉴权并独占唯一文件传输槽位 */
web_file_guard_result_t web_file_transfer_acquire(httpd_req_t *request);

/** @brief 释放当前文件传输槽位并刷新活动会话时间 */
void web_file_transfer_release(void);

/** @brief 把传输缓冲区发布到 Service 所有权 */
bool web_file_publish_transfer_buffer(uint8_t *buffer);

/** @brief 查询停止流程是否已经取消当前传输 */
bool web_file_transfer_is_cancelled(void);

/** @brief 读取单路径查询参数并映射到固定文件系统 */
web_file_operation_result_t web_file_read_and_map_path(httpd_req_t *request, char *logical, size_t logical_size,
                                                       char *filesystem, size_t filesystem_size);

/** @brief 读取移动操作的源路径和目标路径并完成映射 */
web_file_operation_result_t web_file_read_and_map_move_paths(httpd_req_t *request,
                                                             web_file_move_workspace_t *workspace);

/** @brief 把传输守卫结果映射为 HTTP 错误响应 */
esp_err_t web_file_send_guard_error(httpd_req_t *request, web_file_guard_result_t result);

/** @brief 把文件操作结果映射为 HTTP 错误响应 */
esp_err_t web_file_send_operation_error(httpd_req_t *request, web_file_operation_result_t result);
