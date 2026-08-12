/**
 * @file display_collection_service.cpp
 * @brief 实现多页面集合 Service 公共 API 与同步事务
 */
#include "display_collection_service.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <new>

#include "device_sd.h"
#include "display_collection_service_internal.hpp"
#include "display_frame_protocol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "utils.h"

/** @brief 日志标签 */
static const char *TAG = "collection_service";

/** @brief 集合 Service 唯一 Runtime */
DisplayCollectionRuntime g_display_collection_runtime;

/** @brief 下载回调上下文 */
struct DisplayCollectionDownloadContext
{
    uint8_t                            *buffer;         /**< 目标缓冲区 */
    size_t                              capacity;       /**< 缓冲区容量 */
    size_t                              size;           /**< 已写长度 */
    display_collection_cancel_cb_t      should_cancel; /**< 取消回调 */
    void                               *cancel_context; /**< 取消上下文 */
};

/**
 * @brief 判断一次同步事务是否已被上层取消
 */
static bool display_collection_is_cancelled(const display_collection_sync_request_t *request)
{
    return request->should_cancel != nullptr && request->should_cancel(request->cancel_context);
}

/**
 * @brief 把流式下载数据追加到单页缓冲区
 */
static esp_err_t display_collection_on_download_data(const uint8_t *data, size_t length,
                                                     void *context)
{
    auto *download = static_cast<DisplayCollectionDownloadContext *>(context);
    if (download == nullptr || (data == nullptr && length > 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (download->should_cancel != nullptr
        && download->should_cancel(download->cancel_context))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (length > download->capacity - download->size)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (length > 0U)
    {
        std::memcpy(download->buffer + download->size, data, length);
        download->size += length;
    }
    return ESP_OK;
}

/**
 * @brief 为 Manifest 页面构造稳定的本地文件路径
 */
static esp_err_t display_collection_make_page(const display_protocol_page_t &protocol,
                                              display_collection_page_t *out_page)
{
    if (out_page == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    std::memset(out_page, 0, sizeof(*out_page));
    out_page->protocol = protocol;
    const int written = std::snprintf(out_page->file_path,
                                      sizeof(out_page->file_path),
                                      "display/pages/%s-%s.ppf",
                                      protocol.page_id,
                                      protocol.content_version);
    return written > 0 && static_cast<size_t>(written) < sizeof(out_page->file_path)
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

/**
 * @brief 尝试恢复 SD 状态并发布到 Runtime
 */
static esp_err_t display_collection_recover_storage(bool *out_has_active)
{
    DisplayCollectionRecoveredState recovered;
    display_collection_snapshot_t   snapshot = {};
    auto &pages = g_display_collection_runtime.workspace->scratch_pages;
    pages = {};
    const esp_err_t error = display_collection_storage_recover(&recovered, &snapshot, &pages);

    xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
    if (error == ESP_OK)
    {
        snapshot.storage_available = true;
        snapshot.last_error        = ESP_OK;
        g_display_collection_runtime.snapshot    = snapshot;
        g_display_collection_runtime.workspace->active_pages = pages;
        g_display_collection_runtime.active_slot = recovered.active_slot;
    }
    else
    {
        g_display_collection_runtime.snapshot.storage_available = false;
        g_display_collection_runtime.snapshot.has_active        = false;
        g_display_collection_runtime.snapshot.page_count        = 0U;
        g_display_collection_runtime.snapshot.last_error        = error;
        g_display_collection_runtime.active_slot                = -1;
    }
    const bool has_active = g_display_collection_runtime.snapshot.has_active;
    xSemaphoreGive(g_display_collection_runtime.lock);
    if (out_has_active != nullptr)
    {
        *out_has_active = has_active;
    }
    return error;
}

esp_err_t display_collection_service_init(void)
{
    if (g_display_collection_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    g_display_collection_runtime.lock = xSemaphoreCreateMutex();
    if (g_display_collection_runtime.lock == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    g_display_collection_runtime.buffer = static_cast<uint8_t *>(heap_caps_malloc(
        DISPLAY_FRAME_PROTOCOL_FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_display_collection_runtime.buffer == nullptr)
    {
        vSemaphoreDelete(g_display_collection_runtime.lock);
        g_display_collection_runtime.lock = nullptr;
        return ESP_ERR_NO_MEM;
    }
    void *workspace_memory = heap_caps_malloc(sizeof(DisplayCollectionWorkspace),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (workspace_memory == nullptr)
    {
        heap_caps_free(g_display_collection_runtime.buffer);
        g_display_collection_runtime.buffer = nullptr;
        vSemaphoreDelete(g_display_collection_runtime.lock);
        g_display_collection_runtime.lock = nullptr;
        return ESP_ERR_NO_MEM;
    }
    g_display_collection_runtime.workspace =
        new (workspace_memory) DisplayCollectionWorkspace();

    g_display_collection_runtime.snapshot.next_refresh_at_utc = 0;
    g_display_collection_runtime.initialized                 = true;
    const esp_err_t recovery_error = display_collection_recover_storage(nullptr);
    if (recovery_error != ESP_OK)
    {
        ESP_LOGW(TAG, "启动时恢复显示集合失败，等待后续同步重试: %s",
                 esp_err_to_name(recovery_error));
    }
    else if (g_display_collection_runtime.snapshot.has_active)
    {
        ESP_LOGI(TAG,
                 "已从 SD 恢复照片集合: collection=%s, generation=%llu, pages=%u",
                 g_display_collection_runtime.snapshot.active_collection,
                 (unsigned long long) g_display_collection_runtime.snapshot.generation,
                 (unsigned int) g_display_collection_runtime.snapshot.page_count);
    }
    else
    {
        ESP_LOGI(TAG, "SD 中暂无可用照片集合，等待首次联网同步");
    }
    return ESP_OK;
}

esp_err_t display_collection_service_sync(const display_collection_sync_request_t *request,
                                          display_collection_sync_result_t *out_result)
{
    if (request == nullptr || out_result == nullptr
        || !protocol_backend_context_is_valid(request->backend) || request->timeout_ms <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_display_collection_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    auto &workspace = *g_display_collection_runtime.workspace;
    display_collection_snapshot_t previous_snapshot;
    auto &previous_pages = workspace.previous_pages;
    int8_t previous_slot;
    xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
    if (g_display_collection_runtime.syncing)
    {
        xSemaphoreGive(g_display_collection_runtime.lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_display_collection_runtime.syncing          = true;
    g_display_collection_runtime.snapshot.syncing = true;
    previous_pages = {};
    previous_snapshot = g_display_collection_runtime.snapshot;
    previous_snapshot.syncing = false;
    previous_pages    = workspace.active_pages;
    previous_slot     = g_display_collection_runtime.active_slot;
    xSemaphoreGive(g_display_collection_runtime.lock);

    ESP_LOGI(TAG,
             "开始同步照片集合: 当前集合=%s, 本地图片=%u 张, generation=%llu",
             previous_snapshot.has_active ? previous_snapshot.active_collection : "无",
             (unsigned int) previous_snapshot.page_count,
             (unsigned long long) previous_snapshot.generation);

    std::memset(out_result, 0, sizeof(*out_result));
    esp_err_t error = ESP_OK;
    if (!previous_snapshot.storage_available)
    {
        error = display_collection_recover_storage(nullptr);
        if (error != ESP_OK)
        {
            out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_STORAGE;
        }
        if (error == ESP_OK)
        {
            xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
            previous_snapshot = g_display_collection_runtime.snapshot;
            previous_snapshot.syncing = false;
            previous_pages = workspace.active_pages;
            previous_slot  = g_display_collection_runtime.active_slot;
            g_display_collection_runtime.syncing          = true;
            g_display_collection_runtime.snapshot.syncing = true;
            xSemaphoreGive(g_display_collection_runtime.lock);
        }
    }

    auto &manifest = workspace.manifest;
    manifest = {};
    if (error == ESP_OK && !display_collection_is_cancelled(request))
    {
        ESP_LOGI(TAG, "开始请求服务端照片 Manifest");
        error = display_protocol_get_manifest_copy(
            request->backend,
            previous_snapshot.has_active ? previous_snapshot.active_collection : nullptr,
            previous_snapshot.has_active ? previous_snapshot.next_refresh_at_utc : 0,
            request->timeout_ms,
            &manifest);
        if (error != ESP_OK)
        {
            out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_BACKEND;
        }
    }
    else if (error == ESP_OK)
    {
        error = ESP_ERR_INVALID_STATE;
        out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_CANCELLED;
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "照片集合同步在 Manifest 阶段失败: %s", esp_err_to_name(error));
    }

    auto &next_pages = workspace.next_pages;
    next_pages = {};
    if (error == ESP_OK && manifest.not_modified)
    {
        out_result->outcome            = DISPLAY_COLLECTION_SYNC_NOT_MODIFIED;
        out_result->next_refresh_at_utc = previous_snapshot.next_refresh_at_utc;
        ESP_LOGI(TAG,
                 "服务端照片集合未变化，本轮无需下载；当前保留 %u 张图片",
                 (unsigned int) previous_snapshot.page_count);
    }
    else if (error == ESP_OK)
    {
        if (previous_snapshot.has_active
            && std::strcmp(manifest.collection_version, previous_snapshot.active_collection) == 0)
        {
            manifest.not_modified = true;
            out_result->outcome = DISPLAY_COLLECTION_SYNC_NOT_MODIFIED;
            out_result->next_refresh_at_utc = manifest.next_refresh_at_utc;
            ESP_LOGI(TAG,
                     "服务端返回的集合版本与本地一致，本轮无需下载: collection=%s, "
                     "本地图片=%u 张",
                     previous_snapshot.active_collection,
                     (unsigned int) previous_snapshot.page_count);
        }
    }
    if (error == ESP_OK && !manifest.not_modified)
    {
        ESP_LOGI(TAG,
                 "收到新照片 Manifest: collection=%s, pages=%u, default_page=%s, "
                 "next_refresh_at=%lld",
                 manifest.collection_version,
                 (unsigned int) manifest.page_count,
                 manifest.default_page,
                 (long long) manifest.next_refresh_at_utc);
        for (uint8_t index = 0U; index < manifest.page_count && error == ESP_OK; ++index)
        {
            error = display_collection_make_page(manifest.pages[index], &next_pages[index]);
            if (error != ESP_OK || display_collection_is_cancelled(request))
            {
                if (error == ESP_OK)
                {
                    error = ESP_ERR_INVALID_STATE;
                    out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_CANCELLED;
                }
                else
                {
                    out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_BACKEND;
                }
                ESP_LOGE(TAG,
                         "[%u/%u] 准备图片同步失败: page_id=%s, error=%s",
                         (unsigned int) (index + 1U),
                         (unsigned int) manifest.page_count,
                         manifest.pages[index].page_id,
                         esp_err_to_name(error));
                break;
            }

            size_t local_size = 0U;
            error = display_collection_storage_read_page(next_pages[index],
                                                         g_display_collection_runtime.buffer,
                                                         DISPLAY_FRAME_PROTOCOL_FILE_SIZE,
                                                         &local_size);
            display_frame_protocol_view_t view;
            if (error == ESP_OK)
            {
                error = display_frame_protocol_validate_borrow(
                    g_display_collection_runtime.buffer,
                    local_size,
                    &next_pages[index].protocol,
                    &view);
            }
            if (error == ESP_OK)
            {
                ++out_result->reused_pages;
                ESP_LOGI(TAG,
                         "[%u/%u] 复用本地图片: page_id=%s, version=%s",
                         (unsigned int) (index + 1U),
                         (unsigned int) manifest.page_count,
                         manifest.pages[index].page_id,
                         manifest.pages[index].content_version);
                continue;
            }
            if (error != ESP_ERR_NOT_FOUND && error != ESP_ERR_INVALID_RESPONSE
                && error != ESP_ERR_INVALID_SIZE)
            {
                out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_STORAGE;
                ESP_LOGE(TAG,
                         "[%u/%u] 读取本地图片失败，无法继续同步: page_id=%s, error=%s",
                         (unsigned int) (index + 1U),
                         (unsigned int) manifest.page_count,
                         manifest.pages[index].page_id,
                         esp_err_to_name(error));
                break;
            }

            error = ESP_OK;

            ESP_LOGI(TAG,
                     "[%u/%u] 开始下载图片: page_id=%s, version=%s",
                     (unsigned int) (index + 1U),
                     (unsigned int) manifest.page_count,
                     manifest.pages[index].page_id,
                     manifest.pages[index].content_version);

            DisplayCollectionDownloadContext download = {};
            download.buffer         = g_display_collection_runtime.buffer;
            download.capacity       = DISPLAY_FRAME_PROTOCOL_FILE_SIZE;
            download.should_cancel  = request->should_cancel;
            download.cancel_context = request->cancel_context;
            transport_http_download_result_t download_result = {};
            error = display_protocol_download_frame_borrow(request->backend,
                                                           manifest.pages[index].frame_url,
                                                           request->timeout_ms,
                                                           display_collection_on_download_data,
                                                           &download,
                                                           &download_result);
            if (error != ESP_OK)
            {
                out_result->failure = display_collection_is_cancelled(request)
                                          ? DISPLAY_COLLECTION_SYNC_FAILURE_CANCELLED
                                          : DISPLAY_COLLECTION_SYNC_FAILURE_BACKEND;
            }
            if (error == ESP_OK
                && (download_result.status_code < 200 || download_result.status_code >= 300
                    || download_result.received_bytes != download.size))
            {
                error = ESP_ERR_INVALID_RESPONSE;
                out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_BACKEND;
            }
            if (error == ESP_OK)
            {
                error = display_frame_protocol_validate_borrow(
                    g_display_collection_runtime.buffer,
                    download.size,
                    &next_pages[index].protocol,
                    &view);
                if (error != ESP_OK)
                {
                    out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_BACKEND;
                }
            }
            if (error == ESP_OK)
            {
                error = display_collection_storage_store_page(next_pages[index],
                                                              g_display_collection_runtime.buffer,
                                                              download.size);
                if (error != ESP_OK)
                {
                    out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_STORAGE;
                }
            }
            if (error == ESP_OK)
            {
                ++out_result->downloaded_pages;
                ESP_LOGI(TAG,
                         "[%u/%u] 图片下载、校验并写入 SD 完成: page_id=%s, bytes=%u",
                         (unsigned int) (index + 1U),
                         (unsigned int) manifest.page_count,
                         manifest.pages[index].page_id,
                         (unsigned int) download.size);
            }
            else
            {
                ESP_LOGE(TAG,
                         "[%u/%u] 图片下载事务失败: page_id=%s, error=%s",
                         (unsigned int) (index + 1U),
                         (unsigned int) manifest.page_count,
                         manifest.pages[index].page_id,
                         esp_err_to_name(error));
            }
        }
    }

    uint64_t new_generation = 0U;
    int8_t   new_slot       = -1;
    if (error == ESP_OK && !manifest.not_modified)
    {
        ESP_LOGI(TAG, "全部图片已就绪，开始原子提交新照片集合");
        error = display_collection_storage_commit(manifest,
                                                  next_pages,
                                                  previous_snapshot,
                                                  previous_slot,
                                                  workspace.scratch_pages,
                                                  &new_generation,
                                                   &new_slot);
        if (error != ESP_OK)
        {
            out_result->failure = DISPLAY_COLLECTION_SYNC_FAILURE_STORAGE;
        }
    }

    display_collection_commit_cb_t callback = nullptr;
    void *callback_context = nullptr;
    display_collection_snapshot_t published = {};
    xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
    if (error == ESP_OK && !manifest.not_modified)
    {
        published.storage_available = true;
        published.has_active        = true;
        published.syncing           = false;
        published.generation        = new_generation;
        utils_copy_string(published.active_collection,
                          sizeof(published.active_collection),
                          manifest.collection_version);
        utils_copy_string(published.previous_collection,
                          sizeof(published.previous_collection),
                          previous_snapshot.has_active ? previous_snapshot.active_collection : "");
        utils_copy_string(published.default_page,
                          sizeof(published.default_page),
                          manifest.default_page);
        published.page_count         = manifest.page_count;
        published.next_refresh_at_utc = manifest.next_refresh_at_utc;
        published.last_error         = ESP_OK;
        g_display_collection_runtime.snapshot    = published;
        workspace.active_pages                   = next_pages;
        g_display_collection_runtime.active_slot = new_slot;
        callback         = g_display_collection_runtime.commit_callback;
        callback_context = g_display_collection_runtime.commit_context;
        out_result->outcome            = DISPLAY_COLLECTION_SYNC_COMMITTED;
        out_result->next_refresh_at_utc = manifest.next_refresh_at_utc;
    }
    else
    {
        g_display_collection_runtime.snapshot.syncing    = false;
        g_display_collection_runtime.snapshot.last_error = error;
        if (error == ESP_OK && manifest.not_modified)
        {
            g_display_collection_runtime.snapshot.next_refresh_at_utc =
                out_result->next_refresh_at_utc;
        }
        published = g_display_collection_runtime.snapshot;
    }
    const bool committed = error == ESP_OK && !manifest.not_modified;
    if (!committed)
    {
        g_display_collection_runtime.syncing = false;
    }
    xSemaphoreGive(g_display_collection_runtime.lock);

    if (committed)
    {
        ESP_LOGI(TAG, "新照片集合已持久化，开始清理不再保留的旧集合文件");
        display_collection_storage_cleanup_obsolete(previous_snapshot.previous_collection,
                                                    previous_pages.data(),
                                                    previous_snapshot.page_count,
                                                    next_pages.data(),
                                                    manifest.page_count,
                                                    workspace.scratch_pages);
        ESP_LOGI(TAG, "旧集合文件清理流程完成，开始通知照片播放链路");
        if (callback != nullptr)
        {
            callback(&published, callback_context);
        }
        xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
        g_display_collection_runtime.syncing = false;
        xSemaphoreGive(g_display_collection_runtime.lock);
        ESP_LOGI(TAG,
                 "照片集合提交完成: collection=%s, generation=%llu, 总计=%u 张, "
                 "下载=%u 张, 复用=%u 张",
                 published.active_collection,
                 (unsigned long long) published.generation,
                 (unsigned int) published.page_count,
                 (unsigned int) out_result->downloaded_pages,
                 (unsigned int) out_result->reused_pages);
    }
    else if (error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "照片集合同步失败，活动集合保持不变: 已下载=%u 张, 已复用=%u 张, error=%s",
                 (unsigned int) out_result->downloaded_pages,
                 (unsigned int) out_result->reused_pages,
                 esp_err_to_name(error));
    }
    return error;
}

esp_err_t display_collection_service_get_snapshot_copy(
    display_collection_snapshot_t *out_snapshot)
{
    if (out_snapshot == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_display_collection_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
    *out_snapshot = g_display_collection_runtime.snapshot;
    xSemaphoreGive(g_display_collection_runtime.lock);
    return ESP_OK;
}

esp_err_t display_collection_service_get_page_copy(uint8_t index,
                                                   display_collection_page_t *out_page)
{
    if (out_page == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_display_collection_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
    if (!g_display_collection_runtime.snapshot.has_active)
    {
        xSemaphoreGive(g_display_collection_runtime.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= g_display_collection_runtime.snapshot.page_count)
    {
        xSemaphoreGive(g_display_collection_runtime.lock);
        return ESP_ERR_INVALID_ARG;
    }
    *out_page = g_display_collection_runtime.workspace->active_pages[index];
    xSemaphoreGive(g_display_collection_runtime.lock);
    return ESP_OK;
}

esp_err_t display_collection_service_find_page_copy(const char *page_id,
                                                    display_collection_page_t *out_page)
{
    if (page_id == nullptr || page_id[0] == '\0' || out_page == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_display_collection_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
    if (!g_display_collection_runtime.snapshot.has_active)
    {
        xSemaphoreGive(g_display_collection_runtime.lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t index = 0U; index < g_display_collection_runtime.snapshot.page_count; ++index)
    {
        if (std::strcmp(g_display_collection_runtime.workspace->active_pages[index].protocol.page_id,
                        page_id)
            == 0)
        {
            *out_page = g_display_collection_runtime.workspace->active_pages[index];
            xSemaphoreGive(g_display_collection_runtime.lock);
            return ESP_OK;
        }
    }
    xSemaphoreGive(g_display_collection_runtime.lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t display_collection_service_set_commit_callback_borrow(
    display_collection_commit_cb_t callback, void *context)
{
    if (!g_display_collection_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
    g_display_collection_runtime.commit_callback = callback;
    g_display_collection_runtime.commit_context  = context;
    xSemaphoreGive(g_display_collection_runtime.lock);
    return ESP_OK;
}

esp_err_t display_collection_service_deinit(void)
{
    if (!g_display_collection_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_display_collection_runtime.lock, portMAX_DELAY);
    if (g_display_collection_runtime.syncing)
    {
        xSemaphoreGive(g_display_collection_runtime.lock);
        return ESP_ERR_INVALID_STATE;
    }
    g_display_collection_runtime.initialized     = false;
    g_display_collection_runtime.commit_callback = nullptr;
    g_display_collection_runtime.commit_context  = nullptr;
    xSemaphoreGive(g_display_collection_runtime.lock);

    heap_caps_free(g_display_collection_runtime.buffer);
    g_display_collection_runtime.buffer      = nullptr;
    g_display_collection_runtime.buffer_size = 0U;
    g_display_collection_runtime.workspace->~DisplayCollectionWorkspace();
    heap_caps_free(g_display_collection_runtime.workspace);
    g_display_collection_runtime.workspace = nullptr;
    vSemaphoreDelete(g_display_collection_runtime.lock);
    g_display_collection_runtime.lock = nullptr;
    g_display_collection_runtime.snapshot = {};
    g_display_collection_runtime.active_slot = -1;
    return ESP_OK;
}
