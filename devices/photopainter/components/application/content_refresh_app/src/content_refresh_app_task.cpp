/**
 * @file content_refresh_app_task.cpp
 * @brief 实现按需联网、状态上传、集合同步、轮询与退避 Task
 */
#include "content_refresh_app_internal.hpp"

#include <algorithm>
#include <cstdint>

#include "device_status_upload_app.h"
#include "display_collection_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "network_manager.h"
#include "remote_log.h"
#include "system_clock.h"

/** @brief 日志标签 */
static const char *TAG = "content_refresh_task";

/** @brief 三次外层错误退避时长 */
static constexpr uint32_t CONTENT_REFRESH_BACKOFF_MS[] = {
    60000U,
    300000U,
    900000U,
};
/** @brief 连续失败超过三次后使用的一小时重试间隔 */
static constexpr uint32_t CONTENT_REFRESH_LONG_FAILURE_RETRY_MS = 3600000U;
/** @brief 绝对计划无法换算或已经过期时的重新查询间隔 */
static constexpr uint32_t CONTENT_REFRESH_SCHEDULE_RETRY_SECONDS = 60U;
/** @brief 关网前等待远端日志同步上传调用退出的上限 */
static constexpr uint32_t CONTENT_REFRESH_REMOTE_LOG_STOP_TIMEOUT_MS = 10000U;

static uint32_t content_refresh_calculate_normal_wait_ms(int64_t next_refresh_at_utc);

/**
 * @brief 发布刷新状态
 */
static void content_refresh_publish(content_refresh_app_state_t state, esp_err_t error,
                                    uint32_t next_retry_ms)
{
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    g_content_refresh_runtime.status.state         = state;
    g_content_refresh_runtime.status.last_error    = error;
    g_content_refresh_runtime.status.next_retry_ms = next_retry_ms;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
}

/**
 * @brief 在状态锁外发布一轮刷新与网络清理均已收敛的事实
 */
static void content_refresh_notify_round(esp_err_t round_error,
                                         bool network_cleanup_succeeded)
{
    content_refresh_app_round_event_t event = {};
    event.round_error = round_error;
    event.network_cleanup_succeeded = network_cleanup_succeeded;

    content_refresh_app_round_cb_t callback;
    void *callback_context;
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    event.next_refresh_at_utc = g_content_refresh_runtime.status.next_refresh_at_utc;
    callback = g_content_refresh_runtime.round_callback;
    callback_context = g_content_refresh_runtime.round_callback_context;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);

    display_collection_snapshot_t snapshot = {};
    if (display_collection_service_get_snapshot_copy(&snapshot) == ESP_OK)
    {
        event.collection_generation = snapshot.generation;
    }
    if (callback != nullptr)
    {
        callback(&event, callback_context);
    }
}

/**
 * @brief 查询跨上下文停止标记
 */
static bool content_refresh_should_stop()
{
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    const bool stop_requested = g_content_refresh_runtime.stop_requested;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    return stop_requested;
}

/**
 * @brief 集合 Service 下载取消查询
 */
static bool content_refresh_collection_should_cancel(void *context)
{
    (void) context;
    return content_refresh_should_stop();
}

/**
 * @brief Network Manager 状态变化快速通知
 */
static void content_refresh_on_network_change(void *context)
{
    (void) context;
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    task = g_content_refresh_runtime.task;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    if (task != nullptr)
    {
        (void) xTaskNotify(task, CONTENT_REFRESH_NOTIFY_NETWORK, eSetBits);
    }
}

/** @brief 网络上线后尽力恢复已配置的远端日志上传 Task */
static void content_refresh_start_remote_log()
{
    remote_log_status_t status = {};
    const esp_err_t status_error = remote_log_get_status_copy(&status);
    if (status_error == ESP_ERR_INVALID_STATE)
    {
        return;
    }
    if (status_error != ESP_OK)
    {
        ESP_LOGW(TAG, "读取远端日志状态失败，本轮继续内容刷新: %s",
                 esp_err_to_name(status_error));
        return;
    }
    if (status.state != REMOTE_LOG_STATE_STOPPED)
    {
        return;
    }

    const esp_err_t start_error = remote_log_start();
    if (start_error != ESP_OK)
    {
        ESP_LOGW(TAG, "恢复远端日志上传失败，本轮继续内容刷新: %s",
                 esp_err_to_name(start_error));
    }
}

/** @brief 关网前同步停止远端日志上传；未初始化或已停止时视为成功 */
static esp_err_t content_refresh_stop_remote_log()
{
    remote_log_status_t status = {};
    const esp_err_t status_error = remote_log_get_status_copy(&status);
    if (status_error == ESP_ERR_INVALID_STATE)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(status_error, TAG, "读取待停止的远端日志状态失败");
    if (status.state == REMOTE_LOG_STATE_STOPPED)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(remote_log_stop(CONTENT_REFRESH_REMOTE_LOG_STOP_TIMEOUT_MS),
                        TAG,
                        "停止远端日志上传失败");
    return ESP_OK;
}

/**
 * @brief 停止当前 Network Manager 会话；已停止时视为成功
 */
static esp_err_t content_refresh_stop_network()
{
    ESP_RETURN_ON_ERROR(content_refresh_stop_remote_log(),
                        TAG,
                        "关网前停止远端日志上传失败");
    network_manager_status_t status;
    ESP_RETURN_ON_ERROR(network_manager_get_status_copy(&status), TAG, "读取待停止的网络状态失败");
    if (status.state == NETWORK_STATE_STOPPED)
    {
        ESP_RETURN_ON_ERROR(network_manager_set_notify_callback_borrow(nullptr, nullptr),
                            TAG,
                            "解除已停止网络会话的通知回调失败");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(network_manager_stop(), TAG, "停止内容刷新网络会话失败");
    return ESP_OK;
}

/**
 * @brief 启动或接管网络会话并等待 ONLINE
 */
static esp_err_t content_refresh_wait_online()
{
    ESP_RETURN_ON_ERROR(
        network_manager_set_notify_callback_borrow(content_refresh_on_network_change, nullptr),
        TAG,
        "注册内容刷新网络通知失败");

    network_manager_status_t status;
    ESP_RETURN_ON_ERROR(network_manager_get_status_copy(&status), TAG, "读取内容刷新网络状态失败");
    if (status.state == NETWORK_STATE_ERROR)
    {
        ESP_LOGW(TAG,
                 "检测到上一轮网络错误，先清理会话再重新连接: %s",
                 esp_err_to_name(status.last_error));
        ESP_RETURN_ON_ERROR(content_refresh_stop_network(), TAG, "清理上一轮网络错误会话失败");
        status.state = NETWORK_STATE_STOPPED;
    }
    if (status.state == NETWORK_STATE_STOPPED)
    {
        ESP_LOGI(TAG, "网络会话尚未运行，正在启动 Wi-Fi 连接");
        ESP_RETURN_ON_ERROR(network_manager_start(), TAG, "启动内容刷新网络会话失败");
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(CONTENT_REFRESH_NETWORK_WAIT_MS);
    while (!content_refresh_should_stop())
    {
        ESP_RETURN_ON_ERROR(network_manager_get_status_copy(&status),
                            TAG,
                            "等待上线时读取网络状态失败");
        if (status.state == NETWORK_STATE_ONLINE)
        {
            ESP_LOGI(TAG, "网络连接阶段完成");
            content_refresh_start_remote_log();
            return ESP_OK;
        }
        if (status.state == NETWORK_STATE_ERROR)
        {
            return status.last_error != ESP_OK ? status.last_error : ESP_FAIL;
        }
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout_ticks)
        {
            return ESP_ERR_TIMEOUT;
        }
        uint32_t notification = 0U;
        (void) xTaskNotifyWait(0U,
                               CONTENT_REFRESH_NOTIFY_NETWORK | CONTENT_REFRESH_NOTIFY_STOP,
                               &notification,
                               std::min(pdMS_TO_TICKS(1000U), timeout_ticks - elapsed));
        if ((notification & CONTENT_REFRESH_NOTIFY_STOP) != 0U)
        {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

/** @brief 网络上线后同步执行一次 SNTP 校时，失败时记录降级事实并继续本轮刷新 */
static void content_refresh_sync_time()
{
    ESP_LOGI(TAG,
             "网络已上线，开始 SNTP 时间同步：server=%s，timeout=%d ms",
             CONFIG_PHOTOPAINTER_SNTP_SERVER,
             CONFIG_PHOTOPAINTER_SNTP_TIMEOUT_MS);
    const esp_err_t error =
        system_clock_sync_from_sntp(CONFIG_PHOTOPAINTER_SNTP_SERVER,
                                    (uint32_t) CONFIG_PHOTOPAINTER_SNTP_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "SNTP 时间同步失败，本轮继续执行状态上传与照片同步：%s",
                 esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG, "SNTP 时间同步完成，继续执行状态上传与照片同步");
}

/** @brief 把服务端绝对 UTC 刷新时间换算为本 Task 保持唤醒时使用的有界等待毫秒数 */
static uint32_t content_refresh_calculate_normal_wait_ms(int64_t next_refresh_at_utc)
{
    uint64_t wait_seconds = CONTENT_REFRESH_SCHEDULE_RETRY_SECONDS;
    system_clock_snapshot_t snapshot = {};
    if (next_refresh_at_utc <= 0
        || system_clock_get_snapshot_copy(&snapshot) != ESP_OK
        || !snapshot.valid)
    {
        ESP_LOGW(TAG,
                 "无法换算服务端绝对刷新时间，%lu 秒后重新查询",
                 (unsigned long) CONTENT_REFRESH_SCHEDULE_RETRY_SECONDS);
    }
    else if (next_refresh_at_utc > (int64_t) snapshot.utc_timestamp)
    {
        wait_seconds =
            (uint64_t) (next_refresh_at_utc - (int64_t) snapshot.utc_timestamp);
    }
    else
    {
        ESP_LOGW(TAG,
                 "服务端绝对刷新时间已经到期，若设备保持唤醒则 %lu 秒后重新查询："
                 "next_refresh_at=%lld, now=%lld",
                 (unsigned long) CONTENT_REFRESH_SCHEDULE_RETRY_SECONDS,
                 (long long) next_refresh_at_utc,
                 (long long) snapshot.utc_timestamp);
    }
    const uint64_t wait_ms = wait_seconds * 1000ULL;
    return wait_ms > UINT32_MAX ? UINT32_MAX : (uint32_t) wait_ms;
}

/**
 * @brief 执行一轮联网、状态上传、集合同步和网络关闭
 *
 * @param[in] round_number 本次启动后的刷新轮次序号
 * @param[out] out_cleanup_failed true 表示 Network Manager 未达到可重启终态
 * @return 本轮网络连接或内容同步结果；网络清理失败时返回清理错误
 */
static esp_err_t content_refresh_run_round(uint32_t round_number, bool *out_cleanup_failed)
{
    *out_cleanup_failed = false;
    ESP_LOGI(TAG,
             "========== 第 %lu 轮内容刷新开始：等待网络上线 ==========",
             (unsigned long) round_number);
    content_refresh_publish(CONTENT_REFRESH_APP_STATE_WAIT_NETWORK, ESP_OK, 0U);
    esp_err_t round_error = content_refresh_wait_online();
    if (round_error == ESP_OK && !content_refresh_should_stop())
    {
        content_refresh_publish(CONTENT_REFRESH_APP_STATE_SYNCING, ESP_OK, 0U);
        content_refresh_sync_time();
    }
    if (round_error == ESP_OK && !content_refresh_should_stop())
    {
        ESP_LOGI(TAG, "开始上传温湿度与电池状态");
        device_status_upload_app_config_t upload = {};
        upload.backend    = &g_content_refresh_runtime.backend;
        upload.timeout_ms = g_content_refresh_runtime.timeout_ms;
        const esp_err_t upload_error = device_status_upload_app_upload(&upload);
        if (upload_error != ESP_OK)
        {
            ESP_LOGW(TAG, "设备状态上传失败，本轮继续同步照片: %s",
                     esp_err_to_name(upload_error));
        }

        ESP_LOGI(TAG, "开始查询服务端照片集合并同步本地图片");
        display_collection_sync_request_t request = {};
        request.backend        = &g_content_refresh_runtime.backend;
        request.timeout_ms     = g_content_refresh_runtime.timeout_ms;
        request.should_cancel  = content_refresh_collection_should_cancel;
        display_collection_sync_result_t result;
        round_error = display_collection_service_sync(&request, &result);
        if (round_error == ESP_OK)
        {
            display_collection_snapshot_t snapshot = {};
            const esp_err_t snapshot_error =
                display_collection_service_get_snapshot_copy(&snapshot);
            taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
            g_content_refresh_runtime.status.next_refresh_at_utc =
                result.next_refresh_at_utc;
            taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
            if (result.outcome == DISPLAY_COLLECTION_SYNC_NOT_MODIFIED)
            {
                if (snapshot_error == ESP_OK)
                {
                    ESP_LOGI(TAG,
                             "照片集合无变化: 本轮下载=0 张, 当前本地=%u 张, "
                             "next_refresh_at=%lld",
                             (unsigned int) snapshot.page_count,
                             (long long) result.next_refresh_at_utc);
                }
                else
                {
                    ESP_LOGI(TAG,
                             "照片集合无变化: 本轮下载=0 张, next_refresh_at=%lld",
                             (long long) result.next_refresh_at_utc);
                }
            }
            else
            {
                ESP_LOGI(TAG,
                         "照片集合同步完成: 总计=%u 张, 本轮下载=%u 张, 本地复用=%u 张, "
                         "集合代数=%llu, next_refresh_at=%lld",
                         (unsigned int) (result.downloaded_pages + result.reused_pages),
                         (unsigned int) result.downloaded_pages,
                         (unsigned int) result.reused_pages,
                         (unsigned long long) (snapshot_error == ESP_OK ? snapshot.generation
                                                                        : 0U),
                         (long long) result.next_refresh_at_utc);
            }
        }
        else
        {
            ESP_LOGW(TAG,
                     "照片集合同步失败，继续保留本地已有图片: %s",
                     esp_err_to_name(round_error));
        }
    }
    else if (round_error == ESP_OK)
    {
        round_error = ESP_ERR_INVALID_STATE;
    }

    content_refresh_publish(CONTENT_REFRESH_APP_STATE_STOPPING, round_error, 0U);
    ESP_LOGI(TAG, "本轮网络操作结束，开始关闭网络会话");
    const esp_err_t stop_error = content_refresh_stop_network();
    if (stop_error != ESP_OK)
    {
        *out_cleanup_failed = true;
        ESP_LOGE(TAG, "关闭网络会话失败: %s", esp_err_to_name(stop_error));
        return stop_error;
    }
    ESP_LOGI(TAG,
             "第 %lu 轮网络会话已关闭，内容刷新完成收尾",
             (unsigned long) round_number);
    return round_error;
}

/**
 * @brief 等待正常调度或退避期限，手动刷新可提前结束
 *
 * Network Manager 在会话关闭期间仍可能发布状态 Notification。本函数会消费并忽略该通知，
 * 每次被提前唤醒后只扣减实际经过的 Tick，禁止把尚未等待的完整分块计入已用时间。
 *
 * @param[in] wait_ms 本轮计划等待毫秒数
 * @return true 等待到期或收到手动刷新；false 收到停止请求
 */
static bool content_refresh_wait_next(uint32_t wait_ms)
{
    uint32_t remaining_ms = wait_ms;
    while (remaining_ms > 0U && !content_refresh_should_stop())
    {
        const uint32_t chunk_ms = std::min(remaining_ms, static_cast<uint32_t>(60000U));
        uint32_t notification = 0U;
        const TickType_t started_tick = xTaskGetTickCount();
        (void) xTaskNotifyWait(CONTENT_REFRESH_NOTIFY_NETWORK,
                               CONTENT_REFRESH_NOTIFY_NETWORK | CONTENT_REFRESH_NOTIFY_MANUAL
                                   | CONTENT_REFRESH_NOTIFY_STOP,
                               &notification,
                               pdMS_TO_TICKS(chunk_ms));
        const TickType_t elapsed_ticks = xTaskGetTickCount() - started_tick;
        const uint64_t elapsed_ms =
            static_cast<uint64_t>(elapsed_ticks) * 1000ULL / configTICK_RATE_HZ;
        remaining_ms = elapsed_ms >= remaining_ms
                           ? 0U
                           : remaining_ms - static_cast<uint32_t>(elapsed_ms);
        if ((notification & CONTENT_REFRESH_NOTIFY_STOP) != 0U
            || content_refresh_should_stop())
        {
            return false;
        }
        if ((notification & CONTENT_REFRESH_NOTIFY_MANUAL) != 0U)
        {
            ESP_LOGI(TAG, "收到手动刷新请求，提前开始下一轮内容刷新");
            return true;
        }
        if ((notification & CONTENT_REFRESH_NOTIFY_NETWORK) != 0U)
        {
            ESP_LOGD(TAG,
                     "忽略调度等待期间的网络状态通知: 实际经过=%llu ms, 剩余=%lu ms",
                     (unsigned long long) elapsed_ms,
                     (unsigned long) remaining_ms);
        }
    }
    return !content_refresh_should_stop();
}

/**
 * @brief 内容刷新 Task 主循环
 */
static void content_refresh_task(void *context)
{
    (void) context;
    ESP_LOGI(TAG, "内容刷新 Task 已启动，首轮将立即执行");
    bool run_immediately = true;
    uint32_t round_sequence = 0U;
    while (!content_refresh_should_stop())
    {
        if (!run_immediately)
        {
            uint32_t wait_ms;
            taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
            wait_ms = g_content_refresh_runtime.status.next_retry_ms;
            taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
            if (!content_refresh_wait_next(wait_ms))
            {
                break;
            }
        }
        run_immediately = false;

        bool cleanup_failed = false;
        ++round_sequence;
        const esp_err_t error = content_refresh_run_round(round_sequence, &cleanup_failed);
        if (cleanup_failed)
        {
            g_content_refresh_runtime.stop_result = error;
            content_refresh_publish(CONTENT_REFRESH_APP_STATE_CLEANUP_FAILED, error, 0U);
            content_refresh_notify_round(error, false);
            break;
        }
        if (content_refresh_should_stop())
        {
            break;
        }
        uint8_t  consecutive_failures;
        uint32_t completed_rounds;
        uint32_t next_retry_ms;
        uint32_t normal_wait_ms = 0U;
        if (error == ESP_OK)
        {
            int64_t next_refresh_at_utc;
            taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
            next_refresh_at_utc =
                g_content_refresh_runtime.status.next_refresh_at_utc;
            taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
            normal_wait_ms = content_refresh_calculate_normal_wait_ms(next_refresh_at_utc);
        }
        taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
        if (error == ESP_OK)
        {
            g_content_refresh_runtime.status.consecutive_failures = 0U;
            ++g_content_refresh_runtime.status.completed_rounds;
            g_content_refresh_runtime.status.next_retry_ms = normal_wait_ms;
            g_content_refresh_runtime.status.state      = CONTENT_REFRESH_APP_STATE_IDLE;
            g_content_refresh_runtime.status.last_error = ESP_OK;
        }
        else
        {
            ++g_content_refresh_runtime.status.consecutive_failures;
            const uint8_t failure = g_content_refresh_runtime.status.consecutive_failures;
            if (failure <= 3U)
            {
                g_content_refresh_runtime.status.next_retry_ms =
                    CONTENT_REFRESH_BACKOFF_MS[failure - 1U];
            }
            else
            {
                g_content_refresh_runtime.status.next_retry_ms =
                    CONTENT_REFRESH_LONG_FAILURE_RETRY_MS;
            }
            g_content_refresh_runtime.status.state      = CONTENT_REFRESH_APP_STATE_BACKOFF;
            g_content_refresh_runtime.status.last_error = error;
        }
        consecutive_failures = g_content_refresh_runtime.status.consecutive_failures;
        completed_rounds     = g_content_refresh_runtime.status.completed_rounds;
        next_retry_ms        = g_content_refresh_runtime.status.next_retry_ms;
        taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
        if (error == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "第 %lu 轮内容刷新成功: 累计成功=%lu 轮, "
                     "若设备保持唤醒则 %lu 秒后再次刷新",
                     (unsigned long) round_sequence,
                     (unsigned long) completed_rounds,
                     (unsigned long) (next_retry_ms / 1000U));
        }
        else
        {
            ESP_LOGW(TAG,
                     "第 %lu 轮内容刷新失败: 连续失败=%u 次, "
                     "若设备保持唤醒则 %lu 秒后重试, "
                     "error=%s",
                     (unsigned long) round_sequence,
                     (unsigned int) consecutive_failures,
                     (unsigned long) (next_retry_ms / 1000U),
                     esp_err_to_name(error));
        }
        content_refresh_notify_round(error, true);
    }

    if (g_content_refresh_runtime.stop_result == ESP_OK)
    {
        const esp_err_t cleanup_error = content_refresh_stop_network();
        g_content_refresh_runtime.stop_result = cleanup_error;
        content_refresh_publish(cleanup_error == ESP_OK ? CONTENT_REFRESH_APP_STATE_STOPPED
                                                        : CONTENT_REFRESH_APP_STATE_CLEANUP_FAILED,
                                cleanup_error,
                                0U);
    }
    xSemaphoreGive(g_content_refresh_runtime.task_stopped);
    vTaskSuspend(nullptr);
}

esp_err_t content_refresh_app_task_start(void)
{
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    g_content_refresh_runtime.stop_requested = false;
    g_content_refresh_runtime.stop_result    = ESP_OK;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    g_content_refresh_runtime.task = nullptr;
    ESP_RETURN_ON_FALSE(xTaskCreate(content_refresh_task,
                                    "content_refresh",
                                    CONTENT_REFRESH_TASK_STACK_SIZE,
                                    nullptr,
                                    CONTENT_REFRESH_TASK_PRIORITY,
                                    &g_content_refresh_runtime.task)
                            == pdPASS,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "创建内容刷新 Task 失败");
    return ESP_OK;
}

esp_err_t content_refresh_app_task_stop(void)
{
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    g_content_refresh_runtime.stop_requested = true;
    TaskHandle_t task = g_content_refresh_runtime.task;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_INVALID_STATE, TAG, "内容刷新 Task 尚未运行");
    (void) xTaskNotify(task, CONTENT_REFRESH_NOTIFY_STOP, eSetBits);
    const uint64_t sntp_timeout_ms =
        static_cast<uint64_t>(CONFIG_PHOTOPAINTER_SNTP_TIMEOUT_MS) * 2ULL;
    const uint64_t longest_block_ms =
        std::max(static_cast<uint64_t>(g_content_refresh_runtime.timeout_ms), sntp_timeout_ms);
    const uint64_t stop_timeout_with_grace_ms =
        longest_block_ms + CONTENT_REFRESH_STOP_GRACE_MS;
    const uint32_t stop_timeout_ms =
        stop_timeout_with_grace_ms > UINT32_MAX
            ? UINT32_MAX
            : static_cast<uint32_t>(stop_timeout_with_grace_ms);
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(g_content_refresh_runtime.task_stopped, pdMS_TO_TICKS(stop_timeout_ms))
            == pdTRUE,
        ESP_ERR_TIMEOUT,
        TAG,
        "等待内容刷新 Task 停止超时");
    vTaskDelete(task);
    taskENTER_CRITICAL(&g_content_refresh_runtime.state_lock);
    g_content_refresh_runtime.task = nullptr;
    taskEXIT_CRITICAL(&g_content_refresh_runtime.state_lock);
    return g_content_refresh_runtime.stop_result;
}
