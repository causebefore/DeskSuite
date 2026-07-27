/**
 * @file content_refresh_app.cpp
 * @brief 实现内容刷新 App 公共生命周期和手动触发入口
 */
#include "content_refresh_app.h"

#include <cstring>

#include "content_refresh_app_internal.hpp"
#include "esp_check.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "content_refresh_app";

/** @brief 内容刷新 App 唯一 Runtime */
ContentRefreshRuntime g_content_refresh_runtime;

esp_err_t content_refresh_app_init(const content_refresh_app_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != nullptr && protocol_backend_context_is_valid(config->backend)
                            && config->timeout_ms > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "内容刷新配置无效");
    ESP_RETURN_ON_FALSE(!g_content_refresh_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新 App 已初始化");

    g_content_refresh_runtime.task_stopped = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(g_content_refresh_runtime.task_stopped != nullptr,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "创建内容刷新停止信号量失败");
    g_content_refresh_runtime.backend      = *config->backend;
    g_content_refresh_runtime.timeout_ms   = config->timeout_ms;
    g_content_refresh_runtime.status       = {};
    g_content_refresh_runtime.status.state = CONTENT_REFRESH_APP_STATE_STOPPED;
    g_content_refresh_runtime.initialized = true;
    return ESP_OK;
}

esp_err_t content_refresh_app_start(void)
{
    ESP_RETURN_ON_FALSE(g_content_refresh_runtime.initialized
                            && g_content_refresh_runtime.task == nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新 App 当前生命周期不允许启动");
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    const bool can_start = g_content_refresh_runtime.status.state
                           == CONTENT_REFRESH_APP_STATE_STOPPED;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    ESP_RETURN_ON_FALSE(can_start, ESP_ERR_INVALID_STATE, TAG, "内容刷新 App 尚未达到可启动状态");
    ESP_RETURN_ON_ERROR(content_refresh_app_task_start(), TAG, "创建内容刷新 Task 失败");
    return ESP_OK;
}

esp_err_t content_refresh_app_request_refresh(void)
{
    ESP_RETURN_ON_FALSE(g_content_refresh_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新 App 尚未初始化");
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    TaskHandle_t task = g_content_refresh_runtime.task;
    const content_refresh_app_state_t state = g_content_refresh_runtime.status.state;
    const bool stop_requested = g_content_refresh_runtime.stop_requested;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    const bool full_refresh_running = state == CONTENT_REFRESH_APP_STATE_WAIT_NETWORK
                                      || state == CONTENT_REFRESH_APP_STATE_SYNCING;
    if (task != nullptr && !stop_requested && full_refresh_running)
    {
        ESP_LOGI(TAG, "完整内容刷新正在执行，本次手动请求已合并到当前轮次");
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(task != nullptr && !stop_requested
                            && state != CONTENT_REFRESH_APP_STATE_CLEANUP_FAILED,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新 App 当前状态不接受手动刷新");
    ESP_RETURN_ON_FALSE(xTaskNotify(task, CONTENT_REFRESH_NOTIFY_MANUAL, eSetBits) == pdPASS,
                        ESP_FAIL,
                        TAG,
                        "提交手动刷新通知失败");
    return ESP_OK;
}

esp_err_t content_refresh_app_get_status_copy(content_refresh_app_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != nullptr,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "内容刷新状态输出指针为空");
    ESP_RETURN_ON_FALSE(g_content_refresh_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新 App 尚未初始化");
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    *out_status = g_content_refresh_runtime.status;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    return ESP_OK;
}

esp_err_t content_refresh_app_set_round_callback_borrow(
    content_refresh_app_round_cb_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(g_content_refresh_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新 App 尚未初始化");
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    g_content_refresh_runtime.round_callback = callback;
    g_content_refresh_runtime.round_callback_context = callback != nullptr ? context : nullptr;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    return ESP_OK;
}

esp_err_t content_refresh_app_stop(void)
{
    ESP_RETURN_ON_FALSE(g_content_refresh_runtime.initialized
                            && g_content_refresh_runtime.task != nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新 App 当前生命周期不允许停止");
    ESP_RETURN_ON_ERROR(content_refresh_app_task_stop(), TAG, "停止内容刷新 Task 失败");
    return ESP_OK;
}

esp_err_t content_refresh_app_deinit(void)
{
    ESP_RETURN_ON_FALSE(g_content_refresh_runtime.initialized
                            && g_content_refresh_runtime.task == nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "内容刷新 App 当前生命周期不允许反初始化");
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    const bool stopped = g_content_refresh_runtime.status.state
                         == CONTENT_REFRESH_APP_STATE_STOPPED;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    ESP_RETURN_ON_FALSE(stopped, ESP_ERR_INVALID_STATE, TAG, "内容刷新 App 尚未达到已停止状态");
    vSemaphoreDelete(g_content_refresh_runtime.task_stopped);
    g_content_refresh_runtime.task_stopped = nullptr;
    g_content_refresh_runtime.backend                 = {};
    g_content_refresh_runtime.timeout_ms              = 0;
    g_content_refresh_runtime.round_callback          = nullptr;
    g_content_refresh_runtime.round_callback_context = nullptr;
    g_content_refresh_runtime.status       = {};
    g_content_refresh_runtime.initialized  = false;
    return ESP_OK;
}
