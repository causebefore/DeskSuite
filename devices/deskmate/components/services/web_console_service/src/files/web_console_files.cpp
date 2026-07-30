/**
 * @file web_console_files.cpp
 * @brief 网页控制台 Files 内部模块的路由与运行期资源生命周期
 */
#include "web_console_files_internal.hpp"

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

esp_err_t web_console_files_prepare_start(void)
{
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

bool web_console_files_transfer_active_locked(void)
{
    return s_files_context.transfer_active;
}

void web_console_files_reset_context(void)
{
    s_files_context = web_console_files_make_initial_context();
}
