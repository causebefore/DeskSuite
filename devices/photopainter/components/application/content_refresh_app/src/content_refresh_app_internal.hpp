/**
 * @file content_refresh_app_internal.hpp
 * @brief 内容刷新 App 私有 Runtime、通知位与 Task 接口
 */
#pragma once

#include "content_refresh_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** @brief 刷新 Task 栈大小 */
static constexpr uint32_t CONTENT_REFRESH_TASK_STACK_SIZE = 6144U;
/** @brief 刷新 Task 优先级 */
static constexpr UBaseType_t CONTENT_REFRESH_TASK_PRIORITY = 3U;
/** @brief 刷新 Task 停止时在最长同步阻塞之外保留的清理余量 */
static constexpr uint32_t CONTENT_REFRESH_STOP_GRACE_MS = 10000U;
/** @brief 单轮等待 Network Manager 达到 ONLINE 的上限 */
static constexpr uint32_t CONTENT_REFRESH_NETWORK_WAIT_MS = 210000U;
/** @brief 手动刷新通知位 */
static constexpr uint32_t CONTENT_REFRESH_NOTIFY_MANUAL = 1UL << 0U;
/** @brief 停止通知位 */
static constexpr uint32_t CONTENT_REFRESH_NOTIFY_STOP = 1UL << 1U;
/** @brief 网络状态变化通知位 */
static constexpr uint32_t CONTENT_REFRESH_NOTIFY_NETWORK = 1UL << 2U;

/** @brief 内容刷新 App 进程期唯一 Runtime */
class ContentRefreshRuntime final
{
public:
    ContentRefreshRuntime() = default;
    ContentRefreshRuntime(const ContentRefreshRuntime &) = delete;
    ContentRefreshRuntime &operator=(const ContentRefreshRuntime &) = delete;
    ContentRefreshRuntime(ContentRefreshRuntime &&) = delete;
    ContentRefreshRuntime &operator=(ContentRefreshRuntime &&) = delete;

    bool initialized = false; /**< 生命周期初始化标记 */
    bool stop_requested = false; /**< 跨上下文取消事实 */
    TaskHandle_t task = nullptr; /**< 刷新 Task */
    SemaphoreHandle_t task_stopped = nullptr; /**< Task 退出握手 */
    portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED; /**< 状态与取消标记锁 */
    protocol_backend_context_t   backend    = {};                    /**< 后端连接与身份配置副本 */
    int                          timeout_ms = 0;                     /**< HTTP 请求超时 */
    content_refresh_app_status_t status     = {};                    /**< 对外状态 */
    content_refresh_app_round_cb_t round_callback = nullptr; /**< 轮次完成回调 */
    void *round_callback_context = nullptr; /**< 轮次完成回调上下文 */
    esp_err_t stop_result = ESP_OK; /**< Task 清理结果 */
};

/** @brief 内容刷新 App 唯一 Runtime */
extern ContentRefreshRuntime g_content_refresh_runtime;

/** @brief 启动刷新 Task */
esp_err_t content_refresh_app_task_start(void);

/** @brief 同步停止刷新 Task */
esp_err_t content_refresh_app_task_stop(void);
