/**
 * @file photo_playback_app_task.cpp
 * @brief 实现照片导航、集合收敛与按键禁止期间的呈现 Task
 */
#include "photo_playback_app_internal.hpp"

#include <cstring>

#include "button_service.h"
#include "display_present_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "utils.h"

/** @brief 日志标签 */
static const char *TAG = "photo_playback_task";

/**
 * @brief 发布状态、错误和当前页面副本
 */
static void photo_playback_publish(photo_playback_app_state_t state, esp_err_t error,
                                   const display_collection_page_t *current, uint8_t index)
{
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.status.state      = state;
    g_photo_playback_runtime.status.last_error = error;
    if (current != nullptr)
    {
        g_photo_playback_runtime.current_page         = *current;
        g_photo_playback_runtime.status.has_current   = true;
        g_photo_playback_runtime.status.current_index = index;
        utils_copy_string(g_photo_playback_runtime.status.current_page_id,
                          sizeof(g_photo_playback_runtime.status.current_page_id),
                          current->protocol.page_id);
        utils_copy_string(g_photo_playback_runtime.status.current_content_version,
                          sizeof(g_photo_playback_runtime.status.current_content_version),
                          current->protocol.content_version);
    }
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
}

/**
 * @brief 发布一次集合显示收敛尝试，并在成功时更新稳定代数
 */
static void photo_playback_notify_collection_settled(uint64_t generation, bool has_content,
                                                     esp_err_t result)
{
    photo_playback_app_collection_settled_cb_t callback;
    void                                      *callback_context;
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    if (result == ESP_OK || (!has_content && result == ESP_ERR_NOT_FOUND))
    {
        g_photo_playback_runtime.status.collection_settled            = true;
        g_photo_playback_runtime.status.settled_collection_generation = generation;
    }
    callback         = g_photo_playback_runtime.collection_settled_callback;
    callback_context = g_photo_playback_runtime.collection_settled_context;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);

    if (callback != nullptr)
    {
        photo_playback_app_collection_settled_event_t event = {};
        event.collection_generation                         = generation;
        event.has_content                                   = has_content;
        event.result                                        = result;
        callback(&event, callback_context);
    }
}

/** @brief 在状态锁外发布一次成功的用户左右导航活动 */
static void photo_playback_notify_activity()
{
    photo_playback_app_activity_cb_t callback;
    void                            *callback_context;
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    callback         = g_photo_playback_runtime.activity_callback;
    callback_context = g_photo_playback_runtime.activity_context;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    if (callback != nullptr)
    {
        callback(callback_context);
    }
}

/**
 * @brief 启动按键扫描并记录所有权
 */
static esp_err_t photo_playback_start_buttons()
{
    if (g_photo_playback_runtime.button_running)
    {
        return ESP_OK;
    }
    const esp_err_t error = button_service_start();
    if (error == ESP_OK)
    {
        g_photo_playback_runtime.button_running = true;
    }
    return error;
}

/**
 * @brief 停止按键扫描并保留长期回调借用
 */
static esp_err_t photo_playback_stop_buttons()
{
    if (!g_photo_playback_runtime.button_running)
    {
        return ESP_OK;
    }
    const esp_err_t error = button_service_stop();
    if (error == ESP_OK)
    {
        g_photo_playback_runtime.button_running = false;
    }
    return error;
}

/**
 * @brief 在页面物理刷新成功后提交一次性确认，并在确认成功后释放回调借用
 */
static void photo_playback_confirm_first_presented()
{
    const photo_playback_app_first_presented_cb_t callback =
        g_photo_playback_runtime.first_presented_callback;
    if (callback == nullptr)
    {
        return;
    }
    const esp_err_t error = callback(g_photo_playback_runtime.first_presented_context);
    if (error == ESP_OK)
    {
        g_photo_playback_runtime.first_presented_callback = nullptr;
        g_photo_playback_runtime.first_presented_context  = nullptr;
        return;
    }
    ESP_LOGW(TAG, "首张页面显示确认失败，将在下次成功刷新后重试: %s", esp_err_to_name(error));
}

/**
 * @brief 在按键停止期间同步呈现页面并始终尝试恢复按键
 */
static esp_err_t photo_playback_present_page(const display_collection_page_t &page, uint8_t index,
                                             uint64_t collection_generation)
{
    bool had_current;
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    had_current = g_photo_playback_runtime.status.has_current;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    ESP_LOGI(TAG,
             "准备显示图片: generation=%llu, index=%u, page_id=%s, version=%s",
             (unsigned long long) collection_generation,
             (unsigned int) (index + 1U),
             page.protocol.page_id,
             page.protocol.content_version);
    photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_PRESENTING, ESP_OK, nullptr, 0U);
    esp_err_t error = photo_playback_stop_buttons();
    if (error != ESP_OK)
    {
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, error, nullptr, 0U);
        photo_playback_notify_collection_settled(collection_generation, true, error);
        return error;
    }

    const esp_err_t present_error =
        display_present_service_present_borrow(page.file_path, &page.protocol);
    const esp_err_t restart_error = photo_playback_start_buttons();
    if (present_error == ESP_OK)
    {
        photo_playback_confirm_first_presented();
        ESP_LOGI(TAG,
                 "图片显示完成: generation=%llu, index=%u, page_id=%s",
                 (unsigned long long) collection_generation,
                 (unsigned int) (index + 1U),
                 page.protocol.page_id);
        if (restart_error != ESP_OK)
        {
            ESP_LOGW(TAG, "图片已显示，但恢复按键扫描失败: %s", esp_err_to_name(restart_error));
        }
        photo_playback_publish(restart_error == ESP_OK ? PHOTO_PLAYBACK_APP_STATE_IDLE
                                                       : PHOTO_PLAYBACK_APP_STATE_ERROR,
                               restart_error,
                               &page,
                               index);
        photo_playback_notify_collection_settled(collection_generation, true, restart_error);
    }
    else
    {
        photo_playback_publish(restart_error == ESP_OK && had_current
                                   ? PHOTO_PLAYBACK_APP_STATE_IDLE
                                   : PHOTO_PLAYBACK_APP_STATE_ERROR,
                               restart_error == ESP_OK ? present_error : restart_error,
                               nullptr,
                               0U);
        photo_playback_notify_collection_settled(collection_generation,
                                                 true,
                                                 restart_error == ESP_OK ? present_error
                                                                         : restart_error);
    }
    return restart_error != ESP_OK ? restart_error : present_error;
}

/**
 * @brief 在活动集合中查找页面 ID 对应索引
 */
static esp_err_t photo_playback_find_index(const char *page_id, uint8_t page_count,
                                           uint8_t *out_index, display_collection_page_t *out_page)
{
    for (uint8_t index = 0U; index < page_count; ++index)
    {
        display_collection_page_t page;
        const esp_err_t           error = display_collection_service_get_page_copy(index, &page);
        if (error != ESP_OK)
        {
            return error;
        }
        if (std::strcmp(page.protocol.page_id, page_id) == 0)
        {
            *out_index = index;
            *out_page  = page;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/** @brief 在照片播放 Task 内同步显示拥有文本副本的模态 ASCII 状态页 */
static esp_err_t photo_playback_present_status_page(const PhotoPlaybackControlMessage &message)
{
    photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_PRESENTING, ESP_OK, nullptr, 0U);
    esp_err_t error = photo_playback_stop_buttons();
    if (error != ESP_OK)
    {
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, error, nullptr, 0U);
        return error;
    }

    display_present_service_ascii_line_t lines[PHOTO_PLAYBACK_APP_STATUS_LINE_MAX] = {};
    for (uint8_t index = 0U; index < message.line_count; ++index)
    {
        lines[index].x_pixels = message.lines[index].x_pixels;
        lines[index].y_pixels = message.lines[index].y_pixels;
        lines[index].text = message.lines[index].text;
        lines[index].scale = message.lines[index].scale;
    }
    const esp_err_t present_error = display_present_service_present_ascii_layout_borrow(
        lines, message.line_count);
    const esp_err_t restart_error = photo_playback_start_buttons();
    error = restart_error != ESP_OK ? restart_error : present_error;
    photo_playback_publish(error == ESP_OK ? PHOTO_PLAYBACK_APP_STATE_MODAL_PAGE
                                           : PHOTO_PLAYBACK_APP_STATE_ERROR,
                           error,
                           nullptr,
                           0U);
    return error;
}

/** @brief 恢复最近成功照片；没有当前页时选择活动集合默认页 */
static esp_err_t photo_playback_restore_current_page()
{
    bool has_current;
    uint8_t current_index;
    uint64_t settled_generation;
    display_collection_page_t page = {};
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    has_current = g_photo_playback_runtime.status.has_current;
    current_index = g_photo_playback_runtime.status.current_index;
    settled_generation = g_photo_playback_runtime.status.settled_collection_generation;
    if (has_current)
    {
        page = g_photo_playback_runtime.current_page;
    }
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);

    display_collection_snapshot_t snapshot = {};
    const esp_err_t snapshot_error = display_collection_service_get_snapshot_copy(&snapshot);
    if (!has_current)
    {
        ESP_RETURN_ON_FALSE(snapshot_error == ESP_OK && snapshot.has_active
                                && snapshot.page_count > 0U,
                            snapshot_error != ESP_OK ? snapshot_error : ESP_ERR_NOT_FOUND,
                            TAG,
                            "没有可恢复的缓存照片");
        ESP_RETURN_ON_ERROR(photo_playback_find_index(snapshot.default_page,
                                                      snapshot.page_count,
                                                      &current_index,
                                                      &page),
                            TAG,
                            "查找待恢复默认照片失败");
    }
    const uint64_t generation = snapshot_error == ESP_OK ? snapshot.generation
                                                          : settled_generation;
    return photo_playback_present_page(page, current_index, generation);
}

/** @brief 执行一条串行显示控制命令并唤醒同步等待方 */
static void photo_playback_execute_control(const PhotoPlaybackControlMessage &message)
{
    const esp_err_t result = message.kind == PhotoPlaybackControlKind::PresentStatus
                                 ? photo_playback_present_status_page(message)
                                 : photo_playback_restore_current_page();
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.control_result = result;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    (void) xSemaphoreGive(g_photo_playback_runtime.control_done);
}

/** @brief 在照片播放 Task 上下文提交一次模态按键动作 */
static void photo_playback_notify_modal_action(photo_playback_app_modal_action_t action)
{
    photo_playback_app_modal_action_cb_t callback;
    void *callback_context;
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    callback = g_photo_playback_runtime.modal_action_callback;
    callback_context = g_photo_playback_runtime.modal_action_context;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    if (callback != nullptr)
    {
        const esp_err_t error = callback(action, callback_context);
        if (error != ESP_OK)
        {
            ESP_LOGW(TAG, "模态按键动作被拒绝: action=%u error=%s",
                     (unsigned int) action, esp_err_to_name(error));
        }
    }
}

/**
 * @brief 收敛到新活动集合，优先保持当前 page_id
 */
static void photo_playback_converge_collection()
{
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.status.collection_settled = false;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);

    display_collection_snapshot_t snapshot = {};
    esp_err_t                     error = display_collection_service_get_snapshot_copy(&snapshot);
    if (error != ESP_OK || !snapshot.has_active || snapshot.page_count == 0U)
    {
        const esp_err_t no_content_error =
            error != ESP_OK
                ? error
                : (snapshot.last_error != ESP_OK ? snapshot.last_error : ESP_ERR_NOT_FOUND);
        if (no_content_error == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGI(TAG, "本地暂无可显示的照片，等待联网同步新集合");
        }
        else
        {
            ESP_LOGW(TAG,
                     "读取可显示照片集合失败: generation=%llu, error=%s",
                     (unsigned long long) snapshot.generation,
                     esp_err_to_name(no_content_error));
        }
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_NO_CONTENT,
                               error == ESP_OK && snapshot.last_error != ESP_OK
                                   ? snapshot.last_error
                                   : (error == ESP_OK ? ESP_ERR_NOT_FOUND : error),
                               nullptr,
                               0U);
        const esp_err_t button_error = photo_playback_start_buttons();
        if (button_error != ESP_OK)
        {
            photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, button_error, nullptr, 0U);
        }
        const esp_err_t settle_error =
            button_error != ESP_OK
                ? button_error
                : (error != ESP_OK
                       ? error
                       : (snapshot.last_error != ESP_OK ? snapshot.last_error : ESP_ERR_NOT_FOUND));
        photo_playback_notify_collection_settled(snapshot.generation, false, settle_error);
        return;
    }

    ESP_LOGI(TAG,
             "开始收敛活动照片集合: collection=%s, generation=%llu, pages=%u, default_page=%s",
             snapshot.active_collection,
             (unsigned long long) snapshot.generation,
             (unsigned int) snapshot.page_count,
             snapshot.default_page);

    char current_id[DISPLAY_PROTOCOL_PAGE_ID_MAX] = {};
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    if (g_photo_playback_runtime.status.has_current)
    {
        utils_copy_string(current_id,
                          sizeof(current_id),
                          g_photo_playback_runtime.status.current_page_id);
    }
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);

    uint8_t                   index = 0U;
    display_collection_page_t page;
    error = current_id[0] != '\0'
                ? photo_playback_find_index(current_id, snapshot.page_count, &index, &page)
                : ESP_ERR_NOT_FOUND;
    if (error == ESP_ERR_NOT_FOUND)
    {
        error =
            photo_playback_find_index(snapshot.default_page, snapshot.page_count, &index, &page);
    }
    if (error != ESP_OK)
    {
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, error, nullptr, 0U);
        const esp_err_t button_error = photo_playback_start_buttons();
        if (button_error != ESP_OK)
        {
            photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, button_error, nullptr, 0U);
        }
        photo_playback_notify_collection_settled(snapshot.generation, true, error);
        return;
    }
    (void) photo_playback_present_page(page, index, snapshot.generation);
}

/**
 * @brief 启动时采用墨水屏残留画面，不重复刷新本地旧集合
 *
 * 墨水屏断电后会保持上一轮画面。这里仅确认当前集合代数已经稳定，新的集合提交仍会走
 * `photo_playback_converge_collection()` 并真正刷新默认页。
 */
static void photo_playback_adopt_retained_display()
{
    display_collection_snapshot_t snapshot = {};
    const esp_err_t snapshot_error = display_collection_service_get_snapshot_copy(&snapshot);
    if (snapshot_error != ESP_OK || !snapshot.has_active || snapshot.page_count == 0U)
    {
        const esp_err_t no_content_error =
            snapshot_error != ESP_OK
                ? snapshot_error
                : (snapshot.last_error != ESP_OK ? snapshot.last_error : ESP_ERR_NOT_FOUND);
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_NO_CONTENT, no_content_error, nullptr, 0U);
        const esp_err_t button_error = photo_playback_start_buttons();
        if (button_error != ESP_OK)
        {
            photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, button_error, nullptr, 0U);
        }
        photo_playback_notify_collection_settled(snapshot.generation,
                                                 false,
                                                 button_error != ESP_OK ? button_error
                                                                        : no_content_error);
        ESP_LOGI(TAG, "本地无活动集合，保留墨水屏现有画面并等待同步");
        return;
    }

    const esp_err_t button_error = photo_playback_start_buttons();
    if (button_error != ESP_OK)
    {
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, button_error, nullptr, 0U);
        photo_playback_notify_collection_settled(snapshot.generation, true, button_error);
        return;
    }
    photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_IDLE, ESP_OK, nullptr, 0U);
    photo_playback_notify_collection_settled(snapshot.generation, true, ESP_OK);
    ESP_LOGI(TAG,
             "启动时保留墨水屏现有画面，跳过本地旧集合刷新: collection=%s, "
             "generation=%llu, pages=%u",
             snapshot.active_collection,
             (unsigned long long) snapshot.generation,
             (unsigned int) snapshot.page_count);
}

/**
 * @brief 执行一个首尾循环导航请求
 */
static void photo_playback_navigate(PhotoPlaybackNavigation navigation)
{
    display_collection_snapshot_t snapshot;
    esp_err_t                     error = display_collection_service_get_snapshot_copy(&snapshot);
    if (error != ESP_OK || !snapshot.has_active || snapshot.page_count == 0U)
    {
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_NO_CONTENT,
                               error == ESP_OK ? ESP_ERR_NOT_FOUND : error,
                               nullptr,
                               0U);
        return;
    }

    char current_id[DISPLAY_PROTOCOL_PAGE_ID_MAX] = {};
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    if (g_photo_playback_runtime.status.has_current)
    {
        utils_copy_string(current_id,
                          sizeof(current_id),
                          g_photo_playback_runtime.status.current_page_id);
    }
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);

    uint8_t                   current_index = 0U;
    display_collection_page_t current_page;
    error = current_id[0] != '\0' ? photo_playback_find_index(current_id,
                                                              snapshot.page_count,
                                                              &current_index,
                                                              &current_page)
                                  : ESP_ERR_NOT_FOUND;
    if (error == ESP_ERR_NOT_FOUND)
    {
        error = photo_playback_find_index(snapshot.default_page,
                                          snapshot.page_count,
                                          &current_index,
                                          &current_page);
    }
    if (error != ESP_OK)
    {
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, error, nullptr, 0U);
        return;
    }
    const uint8_t next_index =
        navigation == PhotoPlaybackNavigation::Next
            ? static_cast<uint8_t>((current_index + 1U) % snapshot.page_count)
            : static_cast<uint8_t>((current_index + snapshot.page_count - 1U)
                                   % snapshot.page_count);
    display_collection_page_t page;
    error = display_collection_service_get_page_copy(next_index, &page);
    if (error == ESP_OK)
    {
        if (photo_playback_present_page(page, next_index, snapshot.generation) == ESP_OK)
        {
            photo_playback_notify_activity();
        }
    }
    else
    {
        photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_ERROR, error, nullptr, 0U);
    }
}

/**
 * @brief 播放 Task 主循环
 */
static void photo_playback_task(void *context)
{
    (void) context;
    if (g_photo_playback_runtime.present_active_on_start)
    {
        ESP_LOGI(TAG, "照片播放 Task 已启动，开始选择当前集合页面");
        photo_playback_converge_collection();
    }
    else
    {
        ESP_LOGI(TAG, "照片播放 Task 已启动，本轮保留墨水屏现有画面");
        photo_playback_adopt_retained_display();
    }
    while (true)
    {
        uint32_t notification = 0U;
        (void) xTaskNotifyWait(0U, UINT32_MAX, &notification, 0U);
        if ((notification & PHOTO_PLAYBACK_NOTIFY_STOP) != 0U)
        {
            break;
        }
        if ((notification & PHOTO_PLAYBACK_NOTIFY_MODAL_LEFT) != 0U)
        {
            photo_playback_notify_modal_action(PHOTO_PLAYBACK_APP_MODAL_ACTION_LEFT);
        }
        if ((notification & PHOTO_PLAYBACK_NOTIFY_MODAL_CONFIRM) != 0U)
        {
            photo_playback_notify_modal_action(PHOTO_PLAYBACK_APP_MODAL_ACTION_CONFIRM);
        }
        if ((notification & PHOTO_PLAYBACK_NOTIFY_REFRESH_REQUEST) != 0U)
        {
            photo_playback_app_refresh_request_cb_t callback;
            void                                   *callback_context;
            taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
            callback         = g_photo_playback_runtime.refresh_request_callback;
            callback_context = g_photo_playback_runtime.refresh_request_context;
            taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
            if (callback != nullptr)
            {
                const esp_err_t error = callback(callback_context);
                if (error != ESP_OK)
                {
                    ESP_LOGW(TAG, "确认键完整内容刷新请求被拒绝: %s", esp_err_to_name(error));
                }
            }
        }
        if ((notification & PHOTO_PLAYBACK_NOTIFY_FIRMWARE_CHECK) != 0U)
        {
            photo_playback_app_firmware_check_request_cb_t callback;
            void *callback_context;
            taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
            callback = g_photo_playback_runtime.firmware_check_request_callback;
            callback_context = g_photo_playback_runtime.firmware_check_request_context;
            taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
            if (callback != nullptr)
            {
                const esp_err_t error = callback(callback_context);
                if (error != ESP_OK)
                {
                    ESP_LOGW(TAG, "左键长按固件检查请求被拒绝: %s", esp_err_to_name(error));
                }
            }
        }
        if ((notification & PHOTO_PLAYBACK_NOTIFY_COLLECTION_CHANGED) != 0U)
        {
            taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
            const bool modal_active = g_photo_playback_runtime.modal_active;
            if (modal_active)
            {
                g_photo_playback_runtime.collection_change_deferred = true;
            }
            taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
            if (!modal_active)
            {
                photo_playback_converge_collection();
            }
        }

        PhotoPlaybackControlMessage control;
        while (xQueueReceive(g_photo_playback_runtime.control_queue, &control, 0U) == pdTRUE)
        {
            photo_playback_execute_control(control);
        }

        PhotoPlaybackNavigationMessage message;
        if (xQueueReceive(g_photo_playback_runtime.navigation_queue, &message, pdMS_TO_TICKS(100U))
            == pdTRUE)
        {
            taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
            const bool modal_active = g_photo_playback_runtime.modal_active;
            taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
            if (!modal_active)
            {
                photo_playback_navigate(message.navigation);
            }
        }
    }

    photo_playback_publish(PHOTO_PLAYBACK_APP_STATE_STOPPING, ESP_OK, nullptr, 0U);
    ESP_LOGI(TAG, "照片播放 Task 开始停止并关闭按键扫描");
    g_photo_playback_runtime.stop_result = photo_playback_stop_buttons();
    photo_playback_publish(g_photo_playback_runtime.stop_result == ESP_OK
                               ? PHOTO_PLAYBACK_APP_STATE_STOPPED
                               : PHOTO_PLAYBACK_APP_STATE_CLEANUP_FAILED,
                           g_photo_playback_runtime.stop_result,
                           nullptr,
                           0U);
    if (g_photo_playback_runtime.stop_result == ESP_OK)
    {
        ESP_LOGI(TAG, "照片播放 Task 已安全停止");
    }
    xSemaphoreGive(g_photo_playback_runtime.task_stopped);
    vTaskSuspend(nullptr);
}

void photo_playback_app_on_button_event(device_button_id_t button, device_button_event_t event,
                                        uint8_t click_count, void *context)
{
    (void) click_count;
    (void) context;
    bool modal_active;
    TaskHandle_t modal_task;
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    modal_active = g_photo_playback_runtime.modal_active;
    modal_task = g_photo_playback_runtime.task;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    if (modal_active)
    {
        if (event == DEVICE_BUTTON_EVENT_CLICK && modal_task != nullptr)
        {
            if (button == DEVICE_BUTTON_LEFT)
            {
                (void) xTaskNotify(modal_task, PHOTO_PLAYBACK_NOTIFY_MODAL_LEFT, eSetBits);
            }
            else if (button == DEVICE_BUTTON_CONFIRM)
            {
                (void) xTaskNotify(modal_task, PHOTO_PLAYBACK_NOTIFY_MODAL_CONFIRM, eSetBits);
            }
        }
        return;
    }
    if (button == DEVICE_BUTTON_LEFT)
    {
        if (event == DEVICE_BUTTON_EVENT_PRESS)
        {
            taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
            g_photo_playback_runtime.left_press_started_at_us = esp_timer_get_time();
            taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
            return;
        }
        if (event == DEVICE_BUTTON_EVENT_RELEASE)
        {
            taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
            g_photo_playback_runtime.left_press_started_at_us = 0;
            taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
            return;
        }
        if (event == DEVICE_BUTTON_EVENT_LONG_PRESS_END)
        {
            const int64_t released_at_us = esp_timer_get_time();
            int64_t       pressed_at_us;
            TaskHandle_t  task;
            taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
            pressed_at_us = g_photo_playback_runtime.left_press_started_at_us;
            g_photo_playback_runtime.left_press_started_at_us = 0;
            task                                              = g_photo_playback_runtime.task;
            taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
            if (pressed_at_us > 0
                && released_at_us - pressed_at_us >= PHOTO_PLAYBACK_FIRMWARE_CHECK_HOLD_US
                && task != nullptr)
            {
                (void) xTaskNotify(task, PHOTO_PLAYBACK_NOTIFY_FIRMWARE_CHECK, eSetBits);
            }
            return;
        }
    }
    if (button == DEVICE_BUTTON_CONFIRM && event == DEVICE_BUTTON_EVENT_LONG_PRESS_END)
    {
        TaskHandle_t task;
        taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
        task = g_photo_playback_runtime.task;
        taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
        if (task != nullptr)
        {
            (void) xTaskNotify(task, PHOTO_PLAYBACK_NOTIFY_REFRESH_REQUEST, eSetBits);
        }
        return;
    }
    if (event != DEVICE_BUTTON_EVENT_CLICK)
    {
        return;
    }
    if (button == DEVICE_BUTTON_LEFT)
    {
        (void) photo_playback_app_request_previous();
    }
    else if (button == DEVICE_BUTTON_RIGHT)
    {
        (void) photo_playback_app_request_next();
    }
}

void photo_playback_app_on_collection_committed(const display_collection_snapshot_t *snapshot,
                                                void                                *context)
{
    (void) context;
    if (snapshot != nullptr)
    {
        ESP_LOGI(TAG,
                 "收到新照片集合提交通知: collection=%s, generation=%llu, pages=%u",
                 snapshot->active_collection,
                 (unsigned long long) snapshot->generation,
                 (unsigned int) snapshot->page_count);
    }
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    task = g_photo_playback_runtime.task;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    if (task != nullptr)
    {
        (void) xTaskNotify(task, PHOTO_PLAYBACK_NOTIFY_COLLECTION_CHANGED, eSetBits);
    }
}

esp_err_t photo_playback_app_task_start(void)
{
    xQueueReset(g_photo_playback_runtime.navigation_queue);
    xQueueReset(g_photo_playback_runtime.control_queue);
    while (xSemaphoreTake(g_photo_playback_runtime.control_done, 0U) == pdTRUE) {}
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.left_press_started_at_us = 0;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.stop_result = ESP_OK;
    g_photo_playback_runtime.task        = nullptr;
    ESP_RETURN_ON_FALSE(xTaskCreate(photo_playback_task,
                                    "photo_playback",
                                    PHOTO_PLAYBACK_TASK_STACK_SIZE,
                                    nullptr,
                                    PHOTO_PLAYBACK_TASK_PRIORITY,
                                    &g_photo_playback_runtime.task)
                            == pdPASS,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "创建照片播放 Task 失败");
    return ESP_OK;
}

esp_err_t photo_playback_app_task_stop(void)
{
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    TaskHandle_t task = g_photo_playback_runtime.task;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_INVALID_STATE, TAG, "照片播放 Task 尚未运行");
    (void) xTaskNotify(task, PHOTO_PLAYBACK_NOTIFY_STOP, eSetBits);
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_photo_playback_runtime.task_stopped,
                                       pdMS_TO_TICKS(PHOTO_PLAYBACK_STOP_TIMEOUT_MS))
                            == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "等待照片播放 Task 停止超时");
    vTaskDelete(task);
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.task = nullptr;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    return g_photo_playback_runtime.stop_result;
}
