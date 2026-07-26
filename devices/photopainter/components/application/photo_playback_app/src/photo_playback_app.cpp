/**
 * @file photo_playback_app.cpp
 * @brief 实现照片播放 App 公共生命周期与导航入口
 */
#include "photo_playback_app.h"

#include <cstring>

#include "button_service.h"
#include "display_collection_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "photo_playback_app_internal.hpp"

/** @brief 日志标签 */
static const char *TAG = "photo_playback_app";

/** @brief 播放 App 唯一 Runtime */
PhotoPlaybackRuntime g_photo_playback_runtime;

/** @brief 串行投递状态页或照片恢复命令并等待物理刷新完成 */
static esp_err_t photo_playback_app_submit_control(const PhotoPlaybackControlMessage &message)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    TaskHandle_t task = g_photo_playback_runtime.task;
    const photo_playback_app_state_t state = g_photo_playback_runtime.status.state;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    ESP_RETURN_ON_FALSE(task != nullptr && state != PHOTO_PLAYBACK_APP_STATE_STOPPING
                            && state != PHOTO_PLAYBACK_APP_STATE_STOPPED
                            && state != PHOTO_PLAYBACK_APP_STATE_CLEANUP_FAILED,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 当前状态不接受显示控制命令");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(g_photo_playback_runtime.control_mutex,
                                       pdMS_TO_TICKS(PHOTO_PLAYBACK_CONTROL_TIMEOUT_MS)) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "等待照片显示控制所有权超时");
    while (xSemaphoreTake(g_photo_playback_runtime.control_done, 0U) == pdTRUE) {}
    if (xQueueSend(g_photo_playback_runtime.control_queue, &message, 0U) != pdTRUE)
    {
        xSemaphoreGive(g_photo_playback_runtime.control_mutex);
        return ESP_ERR_TIMEOUT;
    }
    (void) xTaskNotify(task, PHOTO_PLAYBACK_NOTIFY_CONTROL, eSetBits);
    if (xSemaphoreTake(g_photo_playback_runtime.control_done,
                       pdMS_TO_TICKS(PHOTO_PLAYBACK_CONTROL_TIMEOUT_MS)) != pdTRUE)
    {
        xSemaphoreGive(g_photo_playback_runtime.control_mutex);
        return ESP_ERR_TIMEOUT;
    }
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    const esp_err_t result = g_photo_playback_runtime.control_result;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    xSemaphoreGive(g_photo_playback_runtime.control_mutex);
    return result;
}

/**
 * @brief 非阻塞投递一个导航请求
 */
static esp_err_t photo_playback_app_request_navigation(PhotoPlaybackNavigation navigation)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");

    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    const bool accepts = g_photo_playback_runtime.status.state == PHOTO_PLAYBACK_APP_STATE_IDLE
                         && !g_photo_playback_runtime.modal_active;
    if (!accepts)
    {
        ++g_photo_playback_runtime.status.rejected_navigation_count;
    }
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    ESP_RETURN_ON_FALSE(accepts, ESP_ERR_INVALID_STATE, TAG, "照片播放 App 当前状态不接受导航请求");

    const PhotoPlaybackNavigationMessage message = { navigation };
    if (xQueueSend(g_photo_playback_runtime.navigation_queue, &message, 0U) != pdTRUE)
    {
        taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
        ++g_photo_playback_runtime.status.rejected_navigation_count;
        taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t photo_playback_app_init(const photo_playback_app_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != nullptr, ESP_ERR_INVALID_ARG, TAG, "照片播放配置为空");
    ESP_RETURN_ON_FALSE(!g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 已初始化");

    esp_err_t ret                        = ESP_OK;
    bool      button_callback_registered = false;
    g_photo_playback_runtime.navigation_queue =
        xQueueCreate(PHOTO_PLAYBACK_NAVIGATION_QUEUE_LENGTH,
                     sizeof(PhotoPlaybackNavigationMessage));
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.navigation_queue != nullptr,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "创建照片导航队列失败");
    g_photo_playback_runtime.control_queue =
        xQueueCreate(PHOTO_PLAYBACK_CONTROL_QUEUE_LENGTH,
                     sizeof(PhotoPlaybackControlMessage));
    ESP_GOTO_ON_FALSE(g_photo_playback_runtime.control_queue != nullptr,
                      ESP_ERR_NO_MEM,
                      cleanup,
                      TAG,
                      "创建照片显示控制队列失败");
    g_photo_playback_runtime.control_mutex = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(g_photo_playback_runtime.control_mutex != nullptr,
                      ESP_ERR_NO_MEM,
                      cleanup,
                      TAG,
                      "创建照片显示控制互斥量失败");
    g_photo_playback_runtime.control_done = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(g_photo_playback_runtime.control_done != nullptr,
                      ESP_ERR_NO_MEM,
                      cleanup,
                      TAG,
                      "创建照片显示控制完成信号量失败");
    g_photo_playback_runtime.task_stopped = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(g_photo_playback_runtime.task_stopped != nullptr,
                      ESP_ERR_NO_MEM,
                      cleanup,
                      TAG,
                      "创建照片播放停止信号量失败");

    ESP_GOTO_ON_ERROR(
        button_service_set_event_callback_borrow(photo_playback_app_on_button_event, nullptr),
        cleanup,
        TAG,
        "注册照片播放按键回调失败");
    button_callback_registered = true;
    ESP_GOTO_ON_ERROR(display_collection_service_set_commit_callback_borrow(
                          photo_playback_app_on_collection_committed,
                          nullptr),
                      cleanup,
                      TAG,
                      "注册照片集合提交回调失败");

    g_photo_playback_runtime.status       = {};
    g_photo_playback_runtime.status.state = PHOTO_PLAYBACK_APP_STATE_STOPPED;
    g_photo_playback_runtime.present_active_on_start = config->present_active_on_start;
    g_photo_playback_runtime.first_presented_callback = config->first_presented_callback;
    g_photo_playback_runtime.first_presented_context  = config->first_presented_context;
    g_photo_playback_runtime.initialized  = true;
    return ESP_OK;

cleanup:
    if (button_callback_registered)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(button_service_set_event_callback_borrow(nullptr, nullptr));
    }
    if (g_photo_playback_runtime.task_stopped != nullptr)
    {
        vSemaphoreDelete(g_photo_playback_runtime.task_stopped);
    }
    if (g_photo_playback_runtime.control_done != nullptr)
    {
        vSemaphoreDelete(g_photo_playback_runtime.control_done);
    }
    if (g_photo_playback_runtime.control_mutex != nullptr)
    {
        vSemaphoreDelete(g_photo_playback_runtime.control_mutex);
    }
    if (g_photo_playback_runtime.control_queue != nullptr)
    {
        vQueueDelete(g_photo_playback_runtime.control_queue);
    }
    vQueueDelete(g_photo_playback_runtime.navigation_queue);
    g_photo_playback_runtime.task_stopped     = nullptr;
    g_photo_playback_runtime.control_done     = nullptr;
    g_photo_playback_runtime.control_mutex    = nullptr;
    g_photo_playback_runtime.control_queue    = nullptr;
    g_photo_playback_runtime.navigation_queue = nullptr;
    return ret;
}

esp_err_t photo_playback_app_start(void)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    const bool can_start = g_photo_playback_runtime.task == nullptr
                           && g_photo_playback_runtime.status.state
                                  == PHOTO_PLAYBACK_APP_STATE_STOPPED;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    ESP_RETURN_ON_FALSE(can_start, ESP_ERR_INVALID_STATE, TAG, "照片播放 App 当前状态不允许启动");
    ESP_RETURN_ON_ERROR(photo_playback_app_task_start(), TAG, "创建照片播放 Task 失败");
    return ESP_OK;
}

esp_err_t photo_playback_app_request_previous(void)
{
    return photo_playback_app_request_navigation(PhotoPlaybackNavigation::Previous);
}

esp_err_t photo_playback_app_request_next(void)
{
    return photo_playback_app_request_navigation(PhotoPlaybackNavigation::Next);
}

esp_err_t photo_playback_app_present_status_page_copy(
    const photo_playback_app_status_line_t *lines, size_t line_count)
{
    ESP_RETURN_ON_FALSE(lines != nullptr && line_count > 0U
                            && line_count <= PHOTO_PLAYBACK_APP_STATUS_LINE_MAX,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "照片状态页行配置无效");
    PhotoPlaybackControlMessage message = {};
    message.kind = PhotoPlaybackControlKind::PresentStatus;
    message.line_count = static_cast<uint8_t>(line_count);
    for (size_t index = 0U; index < line_count; ++index)
    {
        const size_t text_length = lines[index].text != nullptr
                                       ? std::strlen(lines[index].text)
                                       : 0U;
        ESP_RETURN_ON_FALSE(text_length > 0U
                                && text_length <= PHOTO_PLAYBACK_APP_STATUS_TEXT_MAX
                                && lines[index].scale > 0U,
                            ESP_ERR_INVALID_ARG,
                            TAG,
                            "照片状态页文本配置无效");
        message.lines[index].x_pixels = lines[index].x_pixels;
        message.lines[index].y_pixels = lines[index].y_pixels;
        message.lines[index].scale = lines[index].scale;
        std::memcpy(message.lines[index].text, lines[index].text, text_length + 1U);
    }
    return photo_playback_app_submit_control(message);
}

esp_err_t photo_playback_app_restore_current_page(void)
{
    PhotoPlaybackControlMessage message = {};
    message.kind = PhotoPlaybackControlKind::RestoreCurrent;
    return photo_playback_app_submit_control(message);
}

esp_err_t photo_playback_app_get_status_copy(photo_playback_app_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != nullptr,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "照片播放状态输出指针为空");
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    *out_status = g_photo_playback_runtime.status;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    return ESP_OK;
}

esp_err_t photo_playback_app_set_refresh_request_callback_borrow(
    photo_playback_app_refresh_request_cb_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.refresh_request_callback = callback;
    g_photo_playback_runtime.refresh_request_context  = callback != nullptr ? context : nullptr;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    return ESP_OK;
}

esp_err_t photo_playback_app_set_firmware_check_request_callback_borrow(
    photo_playback_app_firmware_check_request_cb_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.firmware_check_request_callback = callback;
    g_photo_playback_runtime.firmware_check_request_context =
        callback != nullptr ? context : nullptr;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    return ESP_OK;
}

esp_err_t photo_playback_app_begin_modal_borrow(
    photo_playback_app_modal_action_cb_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(callback != nullptr, ESP_ERR_INVALID_ARG, TAG, "模态按键回调为空");
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    const bool running = g_photo_playback_runtime.task != nullptr;
    if (running)
    {
        g_photo_playback_runtime.modal_action_callback = callback;
        g_photo_playback_runtime.modal_action_context = context;
        g_photo_playback_runtime.modal_active = true;
        g_photo_playback_runtime.left_press_started_at_us = 0;
    }
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    ESP_RETURN_ON_FALSE(running, ESP_ERR_INVALID_STATE, TAG, "照片播放 Task 尚未运行");
    return ESP_OK;
}

esp_err_t photo_playback_app_end_modal(void)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    TaskHandle_t task;
    bool collection_change_deferred;
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.modal_active = false;
    g_photo_playback_runtime.modal_action_callback = nullptr;
    g_photo_playback_runtime.modal_action_context = nullptr;
    g_photo_playback_runtime.left_press_started_at_us = 0;
    collection_change_deferred = g_photo_playback_runtime.collection_change_deferred;
    g_photo_playback_runtime.collection_change_deferred = false;
    task = g_photo_playback_runtime.task;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    if (collection_change_deferred && task != nullptr)
    {
        (void) xTaskNotify(task, PHOTO_PLAYBACK_NOTIFY_COLLECTION_CHANGED, eSetBits);
    }
    return ESP_OK;
}

esp_err_t photo_playback_app_set_collection_settled_callback_borrow(
    photo_playback_app_collection_settled_cb_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.collection_settled_callback = callback;
    g_photo_playback_runtime.collection_settled_context = callback != nullptr ? context : nullptr;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    return ESP_OK;
}

esp_err_t photo_playback_app_set_activity_callback_borrow(
    photo_playback_app_activity_cb_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 尚未初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    g_photo_playback_runtime.activity_callback = callback;
    g_photo_playback_runtime.activity_context = callback != nullptr ? context : nullptr;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    return ESP_OK;
}

esp_err_t photo_playback_app_stop(void)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized
                            && g_photo_playback_runtime.task != nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 当前生命周期不允许停止");
    ESP_RETURN_ON_ERROR(photo_playback_app_task_stop(), TAG, "停止照片播放 Task 失败");
    return ESP_OK;
}

esp_err_t photo_playback_app_deinit(void)
{
    ESP_RETURN_ON_FALSE(g_photo_playback_runtime.initialized
                            && g_photo_playback_runtime.task == nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "照片播放 App 当前生命周期不允许反初始化");
    taskENTER_CRITICAL(&g_photo_playback_runtime.state_lock);
    const bool stopped = g_photo_playback_runtime.status.state == PHOTO_PLAYBACK_APP_STATE_STOPPED;
    taskEXIT_CRITICAL(&g_photo_playback_runtime.state_lock);
    ESP_RETURN_ON_FALSE(stopped, ESP_ERR_INVALID_STATE, TAG, "照片播放 App 尚未达到已停止状态");

    ESP_RETURN_ON_ERROR(display_collection_service_set_commit_callback_borrow(nullptr, nullptr),
                        TAG,
                        "解除照片集合提交回调失败");
    ESP_RETURN_ON_ERROR(button_service_set_event_callback_borrow(nullptr, nullptr),
                        TAG,
                        "解除照片播放按键回调失败");
    vSemaphoreDelete(g_photo_playback_runtime.task_stopped);
    vSemaphoreDelete(g_photo_playback_runtime.control_done);
    vSemaphoreDelete(g_photo_playback_runtime.control_mutex);
    vQueueDelete(g_photo_playback_runtime.control_queue);
    vQueueDelete(g_photo_playback_runtime.navigation_queue);
    g_photo_playback_runtime.task_stopped     = nullptr;
    g_photo_playback_runtime.control_done     = nullptr;
    g_photo_playback_runtime.control_mutex    = nullptr;
    g_photo_playback_runtime.control_queue    = nullptr;
    g_photo_playback_runtime.navigation_queue = nullptr;
    g_photo_playback_runtime.current_page     = {};
    g_photo_playback_runtime.present_active_on_start = true;
    g_photo_playback_runtime.first_presented_callback = nullptr;
    g_photo_playback_runtime.first_presented_context  = nullptr;
    g_photo_playback_runtime.refresh_request_callback = nullptr;
    g_photo_playback_runtime.refresh_request_context  = nullptr;
    g_photo_playback_runtime.firmware_check_request_callback = nullptr;
    g_photo_playback_runtime.firmware_check_request_context = nullptr;
    g_photo_playback_runtime.modal_active = false;
    g_photo_playback_runtime.collection_change_deferred = false;
    g_photo_playback_runtime.modal_action_callback = nullptr;
    g_photo_playback_runtime.modal_action_context = nullptr;
    g_photo_playback_runtime.control_result = ESP_OK;
    g_photo_playback_runtime.left_press_started_at_us = 0;
    g_photo_playback_runtime.collection_settled_callback = nullptr;
    g_photo_playback_runtime.collection_settled_context = nullptr;
    g_photo_playback_runtime.activity_callback = nullptr;
    g_photo_playback_runtime.activity_context = nullptr;
    g_photo_playback_runtime.status           = {};
    g_photo_playback_runtime.initialized      = false;
    return ESP_OK;
}
