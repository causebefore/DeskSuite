/**
 * @file web_file_service_mutation.cpp
 * @brief 网页文件服务的目录创建、文件移动和删除实现
 */
#include "web_file_service_transfer.hpp"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"

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
 * @return ESP_OK 响应成功；其他错误码表示请求被拒绝或发送失败
 */
esp_err_t web_file_handle_mutation(httpd_req_t *request)
{
    if (request == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    web_file_mutation_t mutation;
    switch (request->method)
    {
        case HTTP_PUT:
            mutation = WEB_FILE_MUTATION_CREATE_DIRECTORY;
            break;
        case HTTP_PATCH:
            mutation = WEB_FILE_MUTATION_MOVE_FILE;
            break;
        case HTTP_DELETE:
            mutation = WEB_FILE_MUTATION_DELETE_ITEM;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
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
            web_file_move_workspace_t *move_workspace = static_cast<web_file_move_workspace_t *>(workspace);
            result                                    = web_file_read_and_map_move_paths(request, move_workspace);
            if (result == WEB_FILE_OPERATION_OK)
            {
                result = web_file_move_file(move_workspace);
            }
        }
        else
        {
            web_file_path_workspace_t *path = static_cast<web_file_path_workspace_t *>(workspace);
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
