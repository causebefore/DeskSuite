/**
 * @file web_console_files.cpp
 * @brief 网页控制台 Files 内部模块的路由与运行期资源生命周期
 */
#include "web_console_files_internal.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

static web_console_files_context_t web_console_files_make_initial_context(void)
{
    web_console_files_context_t context{};
    context.active_transfer_socket = -1;
    return context;
}

web_console_files_context_t s_files_context = web_console_files_make_initial_context();

static constexpr web_console_route_t s_files_routes[] = {
    { .uri = "/api/files", .method = HTTP_GET, .handle = web_file_handle_files_get },
    { .uri = "/api/file", .method = HTTP_GET, .handle = web_file_handle_file_get },
    { .uri = "/api/file", .method = HTTP_PUT, .handle = web_file_handle_file_put },
    { .uri = "/api/directory", .method = HTTP_PUT, .handle = web_file_handle_mutation },
    { .uri = "/api/file", .method = HTTP_PATCH, .handle = web_file_handle_mutation },
    { .uri = "/api/file", .method = HTTP_DELETE, .handle = web_file_handle_mutation },
};

static_assert(sizeof(s_files_routes) / sizeof(s_files_routes[0]) == WEB_CONSOLE_FILES_ROUTE_COUNT,
              "Files 路由数量必须与固定槽配置一致");

const web_console_route_t *web_console_files_get_routes(size_t *out_count)
{
    if (out_count == NULL)
    {
        return NULL;
    }
    *out_count = WEB_CONSOLE_FILES_ROUTE_COUNT;
    return s_files_routes;
}

/**
 * @brief 校验挂载根路径是否为可安全拼接的绝对路径
 *
 * @param[in] mount_root 待校验路径
 * @param[out] out_length 路径长度
 * @return true 路径有效；false 路径为空、过长、非绝对或含禁用字节
 */
static bool web_console_files_mount_root_is_valid(const char *mount_root, size_t *out_length)
{
    if (mount_root == NULL || out_length == NULL)
    {
        return false;
    }
    const size_t length = strnlen(mount_root, WEB_CONSOLE_FILES_MOUNT_ROOT_MAX_LENGTH + 1U);
    if (length == 0U || length > WEB_CONSOLE_FILES_MOUNT_ROOT_MAX_LENGTH || mount_root[0] != '/'
        || (length > 1U && mount_root[length - 1U] == '/'))
    {
        return false;
    }
    for (size_t offset = 0U; offset < length; ++offset)
    {
        const unsigned char value = (unsigned char) mount_root[offset];
        if (value == '\\' || value <= 0x1FU || value == 0x7FU)
        {
            return false;
        }
    }
    if (length > 1U)
    {
        size_t segment_start = 1U;
        for (size_t offset = 1U; offset <= length; ++offset)
        {
            if (offset != length && mount_root[offset] != '/')
            {
                continue;
            }
            const size_t segment_size = offset - segment_start;
            if (segment_size == 0U || (segment_size == 1U && mount_root[segment_start] == '.')
                || (segment_size == 2U && mount_root[segment_start] == '.'
                    && mount_root[segment_start + 1U] == '.'))
            {
                return false;
            }
            segment_start = offset + 1U;
        }
    }
    *out_length = length;
    return true;
}

/**
 * @brief 校验工作目录名是单个、非特殊路径段
 *
 * @param[in] workspace_name 待校验名称
 * @param[out] out_length 名称长度
 * @return true 名称有效；false 名称为空、过长或可改变路径解析语义
 */
static bool web_console_files_workspace_name_is_valid(const char *workspace_name, size_t *out_length)
{
    if (workspace_name == NULL || out_length == NULL)
    {
        return false;
    }
    const size_t length = strnlen(workspace_name, WEB_CONSOLE_FILES_WORKSPACE_NAME_MAX_LENGTH + 1U);
    if (length == 0U || length > WEB_CONSOLE_FILES_WORKSPACE_NAME_MAX_LENGTH
        || (length == 1U && workspace_name[0] == '.')
        || (length == 2U && workspace_name[0] == '.' && workspace_name[1] == '.'))
    {
        return false;
    }
    for (size_t offset = 0U; offset < length; ++offset)
    {
        const unsigned char value = (unsigned char) workspace_name[offset];
        if (value == '/' || value == '\\' || value <= 0x1FU || value == 0x7FU)
        {
            return false;
        }
    }
    *out_length = length;
    return true;
}

/** @brief 向固定缓冲区拼接事务工作目录下的单个固定产物名。 */
static bool web_console_files_build_transaction_path(char *output, size_t output_size, const char *directory,
                                                     const char *name)
{
    const int length = snprintf(output, output_size, "%s/%s", directory, name);
    return length >= 0 && (size_t) length < output_size;
}

esp_err_t web_console_files_configure_borrow(const web_console_files_config_t *config)
{
    size_t mount_root_length;
    size_t workspace_name_length;
    if (config == NULL || config->storage.get_capacity_copy == NULL || config->upload_max_bytes == 0U
        || config->upload_max_bytes > SIZE_MAX
        || config->reserved_free_bytes > UINT64_MAX - config->upload_max_bytes
        || !web_console_files_mount_root_is_valid(config->storage.mount_root, &mount_root_length)
        || !web_console_files_workspace_name_is_valid(config->workspace_name, &workspace_name_length))
    {
        return ESP_ERR_INVALID_ARG;
    }

    web_console_files_context_t next = web_console_files_make_initial_context();
    memcpy(next.mount_root, config->storage.mount_root, mount_root_length + 1U);
    memcpy(next.workspace_name, config->workspace_name, workspace_name_length + 1U);
    next.get_capacity_copy   = config->storage.get_capacity_copy;
    next.storage_context     = config->storage.context;
    next.upload_max_bytes    = config->upload_max_bytes;
    next.reserved_free_bytes = config->reserved_free_bytes;

    const char *separator = mount_root_length == 1U ? "" : "/";
    const int   directory_length =
        snprintf(next.transaction_directory,
                 sizeof(next.transaction_directory),
                 "%s%s%s",
                 next.mount_root,
                 separator,
                 next.workspace_name);
    if (directory_length < 0 || (size_t) directory_length >= sizeof(next.transaction_directory)
        || !web_console_files_build_transaction_path(
            next.transaction_part, sizeof(next.transaction_part), next.transaction_directory, "upload.part")
        || !web_console_files_build_transaction_path(
            next.transaction_backup, sizeof(next.transaction_backup), next.transaction_directory, "upload.bak")
        || !web_console_files_build_transaction_path(
            next.transaction_journal, sizeof(next.transaction_journal), next.transaction_directory, "transaction")
        || !web_console_files_build_transaction_path(
            next.transaction_new, sizeof(next.transaction_new), next.transaction_directory, "transaction.new"))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    next.configured = true;
    s_files_context = next;
    return ESP_OK;
}

esp_err_t web_console_files_prepare_start(void)
{
    if (!s_files_context.configured)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return web_file_transaction_recover();
}

void web_console_files_cleanup_after_handlers(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    uint8_t *buffer                        = s_files_context.transfer_buffer;
    s_files_context.transfer_buffer        = NULL;
    s_files_context.transfer_active        = false;
    s_files_context.active_transfer_socket = -1;
    xSemaphoreGive(s_context.lock);

    if (buffer != NULL)
    {
        heap_caps_free(buffer);
    }
}

bool web_console_files_is_idle_locked(void)
{
    return !s_files_context.transfer_active && s_files_context.active_transfer_socket == -1
           && s_files_context.transfer_buffer == NULL;
}

esp_err_t web_console_files_get_capacity_copy(web_console_files_capacity_t *out_capacity)
{
    if (out_capacity == NULL || s_context.lock == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    const web_console_files_get_capacity_copy_cb_t get_capacity_copy = s_files_context.get_capacity_copy;
    void                                         *storage_context   = s_files_context.storage_context;
    const bool                                    configured        = s_files_context.configured;
    xSemaphoreGive(s_context.lock);

    if (!configured || get_capacity_copy == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *out_capacity          = {};
    const esp_err_t error = get_capacity_copy(storage_context, out_capacity);
    if (error != ESP_OK)
    {
        *out_capacity = {};
        return error;
    }
    if (out_capacity->free_bytes > out_capacity->total_bytes)
    {
        *out_capacity = {};
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

void web_console_files_reset_context(void)
{
    s_files_context = web_console_files_make_initial_context();
}
