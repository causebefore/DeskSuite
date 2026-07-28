/*
 * 文件职责：拥有网页文件管理一次性 Task，并执行存储、网络租约、运行链路和 Service 生命周期。
 */
#include "app_web_file_internal.hpp"

#include "app_network.h"
#include "connect.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "system_filesystem.h"
#include "task_stack_stats.h"
#include "web_file_service.h"

#define APP_WEB_FILE_LEASE_TIMEOUT_MS         1000U
#define APP_WEB_FILE_LEASE_ACQUIRE_TIMEOUT_MS 20000U
#define APP_WEB_FILE_NETWORK_WAIT_MS          ((uint32_t) CONFIG_DESKMATE_WEB_FILE_NETWORK_WAIT_MS)
#define APP_WEB_FILE_NETWORK_POLL_MS          100U
#define APP_WEB_FILE_STOP_TIMEOUT_MS          6000U
#define APP_WEB_FILE_TASK_STACK_SIZE          4096U
#define APP_WEB_FILE_TASK_PRIORITY            4U
#define APP_WEB_FILE_PRESENTATION_RETRY_MS    50U

static const char *TAG          = "app_web_file";

static portMUX_TYPE s_task_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool         s_task_creation_pending;
static uint64_t     s_stop_request_sequence;
static uint64_t     s_consumed_stop_request_sequence;
static bool         s_link_change_pending;

static esp_err_t resume_pending_stop_request(void);

/** @brief 把展示重试间隔换算为至少一个 FreeRTOS Tick */
static TickType_t presentation_retry_ticks(void)
{
    const TickType_t ticks = pdMS_TO_TICKS(APP_WEB_FILE_PRESENTATION_RETRY_MS);
    return ticks == 0 ? 1 : ticks;
}

/** @brief 查询是否存在尚未由清理轮次消费的停止请求 */
static bool has_unconsumed_stop_request(void)
{
    bool pending;
    taskENTER_CRITICAL(&s_task_lock);
    pending = s_stop_request_sequence > s_consumed_stop_request_sequence;
    taskEXIT_CRITICAL(&s_task_lock);
    return pending;
}

/** @brief 原子消费已经合并的最新链路变化通知 */
static bool take_link_change_pending(void)
{
    taskENTER_CRITICAL(&s_task_lock);
    const bool pending    = s_link_change_pending;
    s_link_change_pending = false;
    taskEXIT_CRITICAL(&s_task_lock);
    return pending;
}

/**
 * @brief 原子记录本轮清理已看到的最新停止请求序列
 */
static void consume_stop_requests_for_round(void)
{
    taskENTER_CRITICAL(&s_task_lock);
    s_consumed_stop_request_sequence = s_stop_request_sequence;
    taskEXIT_CRITICAL(&s_task_lock);
}

/**
 * @brief 读取 Service 生命周期快照并记录读取错误
 *
 * @param[out] out_status Service 运行摘要
 * @return ESP_OK 快照有效；其他值为 Service 状态读取错误
 */
static esp_err_t get_service_status(web_file_service_status_t *out_status)
{
    const esp_err_t error = web_file_service_get_status_copy(out_status);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "读取网页文件 Service 状态失败: %s", esp_err_to_name(error));
    }
    return error;
}

/**
 * @brief 释放当前 Web 文件网络租约，失败时保留代次供后续停止意图重试
 *
 * @return ESP_OK 不再持有租约；其他值表示租约所有权尚未安全收敛
 */
static esp_err_t release_network_lease(void)
{
    const uint32_t generation = app_web_file_internal_get_lease_generation();
    if (generation == 0U)
    {
        return ESP_OK;
    }

    const esp_err_t error = app_network_release_web_file_lease(generation, APP_WEB_FILE_LEASE_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "释放网页文件网络租约失败: generation=%lu err=%s",
                 (unsigned long) generation,
                 esp_err_to_name(error));
        return error;
    }

    app_web_file_internal_set_lease_generation(0U);
    return ESP_OK;
}

/**
 * @brief 确认 Network Manager 在线且当前 STA 仍关联并持有 IPv4
 *
 * @return ESP_OK 两份快照均满足启动条件；ESP_ERR_INVALID_STATE 当前不具备本地访问条件；
 *         其他值为快照读取错误
 */
static esp_err_t confirm_online_ipv4(void)
{
    network_manager_status_t manager_status;
    esp_err_t                error = network_manager_get_status_copy(&manager_status);
    if (error != ESP_OK)
    {
        return error;
    }
    if (manager_status.state != NETWORK_STATE_ONLINE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    connect_link_info_t link;
    error = connect_get_link_snapshot_copy(&link);
    if (error != ESP_OK)
    {
        return error;
    }
    if (!link.associated || !link.has_ipv4 || link.ip[0] == '\0')
    {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/**
 * @brief 在有限时间内等待 Network Manager 发布 ONLINE
 *
 * 唤醒重连和上线后的 Dashboard 同步期间网络尚未就绪或网络任务被占用，
 * 启动前先做有界等待，避免首次启动被即时判定拒绝。
 *
 * @param[in] timeout_ms 最长等待时间
 * @return ESP_OK 已在线；ESP_ERR_TIMEOUT 等待超时；其他值为当前网络状态对应错误码
 */
static esp_err_t wait_for_network_online(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;)
    {
        if (has_unconsumed_stop_request())
        {
            return ESP_ERR_INVALID_STATE;
        }

        network_manager_status_t status{};
        const esp_err_t          error = network_manager_get_status_copy(&status);
        if (error != ESP_OK)
        {
            return error;
        }
        switch (status.state)
        {
            case NETWORK_STATE_ONLINE:
                return ESP_OK;
            case NETWORK_STATE_PROVISIONING:
            case NETWORK_STATE_VALIDATING:
            case NETWORK_STATE_STOPPED:
            case NETWORK_STATE_STOPPING:
                return ESP_ERR_INVALID_STATE;
            case NETWORK_STATE_ERROR:
                return status.last_error != ESP_OK ? status.last_error : ESP_FAIL;
            case NETWORK_STATE_CONNECTING:
            case NETWORK_STATE_RETRY_WAIT:
            default:
                break;
        }

        if ((int32_t) (deadline - xTaskGetTickCount()) <= 0)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_WEB_FILE_NETWORK_POLL_MS));
    }
}

/**
 * @brief 停止 Service、反初始化固定资源，再释放网络租约
 *
 * 任何一步未安全完成都会保留当前及后续资源所有权，调用方随后发布可重试错误态。
 *
 * @return ESP_OK Service 与租约均已释放；其他值为首个未收敛步骤错误
 */
static esp_err_t stop_owned_resources(void)
{
    web_file_service_status_t service_status;
    esp_err_t                 error = get_service_status(&service_status);
    if (error != ESP_OK)
    {
        return error;
    }

    if (service_status.state == WEB_FILE_SERVICE_STATE_RUNNING
        || service_status.state == WEB_FILE_SERVICE_STATE_STARTING
        || service_status.state == WEB_FILE_SERVICE_STATE_STOPPING
        || service_status.state == WEB_FILE_SERVICE_STATE_CLEANUP_FAILED)
    {
        app_web_file_internal_set_service_cleanup_required(true);
        error = web_file_service_stop(APP_WEB_FILE_STOP_TIMEOUT_MS);
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "网页文件 Service 有界停止失败: %s", esp_err_to_name(error));
            return error;
        }
        service_status.state = WEB_FILE_SERVICE_STATE_INITIALIZED;
    }

    if (service_status.state == WEB_FILE_SERVICE_STATE_INITIALIZED)
    {
        app_web_file_internal_set_service_cleanup_required(true);
        error = web_file_service_deinit();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "反初始化网页文件 Service 失败: %s", esp_err_to_name(error));
            return error;
        }
        service_status.state = WEB_FILE_SERVICE_STATE_UNINITIALIZED;
    }

    if (service_status.state != WEB_FILE_SERVICE_STATE_UNINITIALIZED)
    {
        app_web_file_internal_set_service_cleanup_required(true);
        return ESP_ERR_INVALID_STATE;
    }

    app_web_file_internal_set_service_cleanup_required(false);
    return release_network_lease();
}

/**
 * @brief 把启动失败按相反顺序回滚；Service 未安全收敛时绝不释放租约
 *
 * @param[in] initialized_this_attempt 本轮是否创建了 Service 固定资源
 * @return ESP_OK 本轮取得的资源已回滚；其他值为清理错误
 */
static esp_err_t rollback_start(bool initialized_this_attempt)
{
    web_file_service_status_t service_status;
    esp_err_t                 error = get_service_status(&service_status);
    if (error != ESP_OK)
    {
        return error;
    }

    if (service_status.state == WEB_FILE_SERVICE_STATE_STARTING
        || service_status.state == WEB_FILE_SERVICE_STATE_RUNNING
        || service_status.state == WEB_FILE_SERVICE_STATE_STOPPING
        || service_status.state == WEB_FILE_SERVICE_STATE_CLEANUP_FAILED)
    {
        app_web_file_internal_set_service_cleanup_required(true);
        error = web_file_service_stop(APP_WEB_FILE_STOP_TIMEOUT_MS);
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚网页文件 Service 失败，保留网络租约: %s", esp_err_to_name(error));
            return error;
        }
        service_status.state = WEB_FILE_SERVICE_STATE_INITIALIZED;
    }

    if (initialized_this_attempt && service_status.state == WEB_FILE_SERVICE_STATE_INITIALIZED)
    {
        app_web_file_internal_set_service_cleanup_required(true);
        error = web_file_service_deinit();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚网页文件 Service 初始化失败，保留网络租约: %s", esp_err_to_name(error));
            return error;
        }
    }

    app_web_file_internal_set_service_cleanup_required(false);
    return release_network_lease();
}

/**
 * @brief 完成一次启动流程，任一前置失败均只回滚本轮实际取得的资源
 *
 * @return ESP_OK 已进入 RUNNING 或停止意图已安全收敛；其他值为启动或回滚错误
 */
static esp_err_t start_owned_resources(void)
{
    system_filesystem_info_t filesystem_info;
    if (!system_filesystem_is_mounted())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = system_filesystem_get_info_copy(&filesystem_info);
    if (error != ESP_OK)
    {
        return error;
    }
    app_web_file_internal_set_capacity(filesystem_info.total_bytes, filesystem_info.free_bytes);

    if (has_unconsumed_stop_request())
    {
        return ESP_ERR_INVALID_STATE;
    }

    app_web_file_internal_publish_state(APP_WEB_FILE_STATE_ACQUIRING_NETWORK, ESP_OK, true);

    error = wait_for_network_online(APP_WEB_FILE_NETWORK_WAIT_MS);
    if (error != ESP_OK)
    {
        return error;
    }

    uint32_t generation = 0U;
    error               = app_network_acquire_web_file_lease(APP_WEB_FILE_LEASE_ACQUIRE_TIMEOUT_MS, &generation);
    if (error != ESP_OK)
    {
        return error;
    }
    app_web_file_internal_set_lease_generation(generation);

    if (has_unconsumed_stop_request())
    {
        return ESP_ERR_INVALID_STATE;
    }

    error = confirm_online_ipv4();
    if (error != ESP_OK)
    {
        const esp_err_t cleanup_error = release_network_lease();
        return cleanup_error != ESP_OK ? cleanup_error : error;
    }

    web_file_service_status_t service_status;
    error = get_service_status(&service_status);
    if (error != ESP_OK)
    {
        const esp_err_t cleanup_error = release_network_lease();
        return cleanup_error != ESP_OK ? cleanup_error : error;
    }

    bool initialized_this_attempt = false;
    if (service_status.state == WEB_FILE_SERVICE_STATE_UNINITIALIZED)
    {
        error = web_file_service_init();
        if (error != ESP_OK)
        {
            const esp_err_t cleanup_error = release_network_lease();
            return cleanup_error != ESP_OK ? cleanup_error : error;
        }
        initialized_this_attempt = true;
    }
    else if (service_status.state != WEB_FILE_SERVICE_STATE_INITIALIZED)
    {
        app_web_file_internal_set_service_cleanup_required(true);
        return ESP_ERR_INVALID_STATE;
    }

    if (has_unconsumed_stop_request())
    {
        return stop_owned_resources();
    }

    app_web_file_internal_publish_state(APP_WEB_FILE_STATE_STARTING_SERVICE, ESP_OK, true);
    error = web_file_service_start();
    if (error != ESP_OK)
    {
        const esp_err_t cleanup_error = rollback_start(initialized_this_attempt);
        return cleanup_error != ESP_OK ? cleanup_error : error;
    }

    app_web_file_internal_set_service_cleanup_required(true);
    if (has_unconsumed_stop_request())
    {
        return ESP_OK;
    }

    error = app_web_file_internal_publish_running_snapshot();
    if (error != ESP_OK)
    {
        const esp_err_t cleanup_error = rollback_start(initialized_this_attempt);
        return cleanup_error != ESP_OK ? cleanup_error : error;
    }
    return ESP_OK;
}

/** @brief 输出最终栈水位并删除已经与全局句柄解绑的当前 Task */
static void delete_detached_task(void)
{
    task_stack_stats_log_now("app_web_file_task");
    vTaskDelete(NULL);
}

/**
 * @brief 若最新停止序列已被当前轮次消费，则原子解绑当前 Task
 *
 * @return true 当前 Task 已解绑，可以删除；false 已收到新的停止意图，应继续清理
 */
static bool detach_task_if_no_stop_retry(void)
{
    bool detached = false;
    taskENTER_CRITICAL(&s_task_lock);
    if (s_stop_request_sequence == s_consumed_stop_request_sequence)
    {
        s_link_change_pending = false;
        s_task                = NULL;
        detached              = true;
    }
    taskEXIT_CRITICAL(&s_task_lock);
    return detached;
}

/** @brief 成功收敛后原子消费全部停止请求并解绑当前 Task */
static void detach_stopped_task(void)
{
    taskENTER_CRITICAL(&s_task_lock);
    s_consumed_stop_request_sequence = s_stop_request_sequence;
    s_link_change_pending            = false;
    s_task                           = NULL;
    taskEXIT_CRITICAL(&s_task_lock);
}

/**
 * @brief 在删除一次性 Task 前确保终态刷新已进入默认 Event Loop
 *
 * 停止序列在每次派发尝试前后都优先检查；若出现更晚停止请求，返回 false 交给清理循环处理，
 * 不会把 Task notification 当成普通派发重试吞掉。本等待只重试事件入队，不读取网络或磁盘。
 *
 * @return true 当前终态刷新已经入队；false 有更晚停止请求需要优先处理
 */
static bool deliver_terminal_status_update(void)
{
    for (;;)
    {
        if (has_unconsumed_stop_request())
        {
            return false;
        }
        if (app_web_file_internal_retry_status_update() == ESP_OK)
        {
            return true;
        }
        if (has_unconsumed_stop_request())
        {
            return false;
        }
        (void) ulTaskNotifyTake(pdTRUE, presentation_retry_ticks());
    }
}

/**
 * @brief 阻塞等待停止请求，并在等待期间合并刷新最新运行链路
 *
 * 停止序列始终优先于链路刷新。链路通知只唤醒 Task；真正的快照读取、URL 更新和 Presenter
 * 推送都在本 Task 上下文完成，不在 `app_network_task` 回调中执行。Event Loop 暂满时使用
 * 有界间隔只重试 pending 状态事件，不轮询网络。
 */
static void wait_for_stop_request(void)
{
    for (;;)
    {
        if (has_unconsumed_stop_request())
        {
            return;
        }

        const esp_err_t dispatch_error = app_web_file_internal_retry_status_update();
        if (has_unconsumed_stop_request())
        {
            return;
        }

        if (take_link_change_pending())
        {
            if (has_unconsumed_stop_request())
            {
                return;
            }
            const esp_err_t error = app_web_file_internal_refresh_running_link();
            if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGW(TAG, "刷新网页文件运行地址失败: %s", esp_err_to_name(error));
            }
            continue;
        }

        const TickType_t wait_ticks = dispatch_error == ESP_OK ? portMAX_DELAY : presentation_retry_ticks();
        (void) ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

/**
 * @brief 一次性拥有网页文件产品状态机的 Application Task
 *
 * Task 先等待创建方发布句柄，再根据入口状态执行启动或清理重试；运行期间阻塞等待合并的
 * 停止或链路 notification，不轮询网络。停止请求优先，链路变化只刷新当前 URL。
 */
static void app_web_file_task(void *arg)
{
    (void) arg;
    (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const app_web_file_state_t entry_state = app_web_file_internal_get_state();
    if (entry_state != APP_WEB_FILE_STATE_STOPPING)
    {
        const esp_err_t start_error = start_owned_resources();
        if (start_error != ESP_OK)
        {
            if (!has_unconsumed_stop_request())
            {
                app_web_file_internal_publish_state(APP_WEB_FILE_STATE_ERROR, start_error, true);
                if (deliver_terminal_status_update() && detach_task_if_no_stop_retry())
                {
                    delete_detached_task();
                    return;
                }
            }
            /* 已有停止意图，或错误发布期间收到清理重试，统一进入停止循环。 */
        }
    }

    wait_for_stop_request();

    for (;;)
    {
        consume_stop_requests_for_round();
        app_web_file_internal_publish_state(APP_WEB_FILE_STATE_STOPPING, ESP_OK, true);
        const esp_err_t stop_error = stop_owned_resources();
        if (stop_error == ESP_OK)
        {
            app_web_file_internal_publish_state(APP_WEB_FILE_STATE_STOPPED, ESP_OK, true);
            if (!deliver_terminal_status_update())
            {
                continue;
            }
            detach_stopped_task();
            delete_detached_task();
            return;
        }

        /*
         * 当前清理轮次已记录消费序列。发布 ERROR 后在同一 Task 锁内比较最新与已消费序列；
         * 若期间到达了更晚的停止请求，则继续下一轮，否则安全解绑并退出。
         */
        app_web_file_internal_publish_state(APP_WEB_FILE_STATE_ERROR, stop_error, true);
        if (deliver_terminal_status_update() && detach_task_if_no_stop_retry())
        {
            delete_detached_task();
            return;
        }
    }
}

/**
 * @brief 创建一次性 Task，并在句柄发布后用 notification 解除其启动门
 *
 * @param[in] for_stop true 表示当前创建由同步停止请求发起；false 表示启动请求
 * @return ESP_OK Task 已创建并唤醒；ESP_ERR_NO_MEM 创建失败
 */
static esp_err_t create_application_task(bool for_stop)
{
    TaskHandle_t     task    = NULL;
    const BaseType_t created = xTaskCreate(app_web_file_task,
                                           "app_web_file_task",
                                           APP_WEB_FILE_TASK_STACK_SIZE,
                                           NULL,
                                           APP_WEB_FILE_TASK_PRIORITY,
                                           &task);
    if (created != pdPASS)
    {
        /*
         * 创建门仍保持占用，且 s_task 从未发布；同步拒绝因此可以在 Presenter 推送互斥量内
         * 清理本请求准备态的 pending，不会与活动 Task 的更高版本并发。
         */
        app_web_file_internal_publish_synchronous_rejection(ESP_ERR_NO_MEM);

        bool concurrent_stop_pending = false;
        taskENTER_CRITICAL(&s_task_lock);
        s_task_creation_pending = false;
        concurrent_stop_pending = !for_stop && s_stop_request_sequence > s_consumed_stop_request_sequence;
        taskEXIT_CRITICAL(&s_task_lock);

        /*
         * 启动 Task 创建窗口内到达的停止意图已经返回 ESP_OK，必须在释放创建门后接续；停止
         * 请求自身的创建失败直接向其调用方返回错误，避免在低内存下递归无限重试。
         */
        if (concurrent_stop_pending)
        {
            (void) resume_pending_stop_request();
        }
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_task_lock);
    s_task                  = task;
    s_task_creation_pending = false;
    s_link_change_pending   = false;
    taskEXIT_CRITICAL(&s_task_lock);
    xTaskNotifyGive(task);
    return ESP_OK;
}

/**
 * @brief 在已占用 Task 创建门后收敛无 Task 的停止请求
 *
 * @return ESP_OK 已停止或清理 Task 已创建；其他值为状态收敛或 Task 创建错误
 */
static esp_err_t prepare_claimed_stop_task(void)
{
    bool            needs_task = false;
    const esp_err_t error      = app_web_file_internal_prepare_stop_retry(&needs_task);
    if (error != ESP_OK || !needs_task)
    {
        taskENTER_CRITICAL(&s_task_lock);
        s_task_creation_pending = false;
        if (error == ESP_OK)
        {
            /* 已处于 STOPPED；成功结果可以消费并发到达的全部停止请求。 */
            s_consumed_stop_request_sequence = s_stop_request_sequence;
        }
        taskEXIT_CRITICAL(&s_task_lock);
        return error;
    }
    return create_application_task(true);
}

/**
 * @brief 在启动准备失败后接续此前并发到达、尚未消费的停止请求
 *
 * @return ESP_OK 无待处理请求或已接续；其他值为清理准备或 Task 创建错误
 */
static esp_err_t resume_pending_stop_request(void)
{
    taskENTER_CRITICAL(&s_task_lock);
    if (s_task != NULL || s_task_creation_pending || s_stop_request_sequence == s_consumed_stop_request_sequence)
    {
        taskEXIT_CRITICAL(&s_task_lock);
        return ESP_OK;
    }
    s_task_creation_pending = true;
    taskEXIT_CRITICAL(&s_task_lock);
    return prepare_claimed_stop_task();
}

esp_err_t app_web_file_task_request_start(void)
{
    taskENTER_CRITICAL(&s_task_lock);
    if (s_task != NULL || s_task_creation_pending || s_stop_request_sequence != s_consumed_stop_request_sequence
        || s_stop_request_sequence == UINT64_MAX)
    {
        taskEXIT_CRITICAL(&s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_task_creation_pending = true;
    taskEXIT_CRITICAL(&s_task_lock);

    const esp_err_t error = app_web_file_internal_prepare_start();
    if (error != ESP_OK)
    {
        bool stop_was_requested;
        taskENTER_CRITICAL(&s_task_lock);
        s_task_creation_pending = false;
        stop_was_requested      = s_stop_request_sequence > s_consumed_stop_request_sequence;
        taskEXIT_CRITICAL(&s_task_lock);
        if (stop_was_requested)
        {
            (void) resume_pending_stop_request();
        }
        return error;
    }
    return create_application_task(false);
}

esp_err_t app_web_file_task_request_stop(void)
{
    const esp_err_t validation_error = app_web_file_internal_validate_stop_request();
    if (validation_error != ESP_OK)
    {
        return validation_error;
    }

    taskENTER_CRITICAL(&s_task_lock);
    if (s_stop_request_sequence == UINT64_MAX)
    {
        taskEXIT_CRITICAL(&s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ++s_stop_request_sequence;
    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
        taskEXIT_CRITICAL(&s_task_lock);
        return ESP_OK;
    }
    if (s_task_creation_pending)
    {
        taskEXIT_CRITICAL(&s_task_lock);
        return ESP_OK;
    }
    s_task_creation_pending = true;
    taskEXIT_CRITICAL(&s_task_lock);
    return prepare_claimed_stop_task();
}

void app_web_file_task_notify_link_change(void)
{
    taskENTER_CRITICAL(&s_task_lock);
    if (s_task != NULL)
    {
        s_link_change_pending = true;
        xTaskNotifyGive(s_task);
    }
    taskEXIT_CRITICAL(&s_task_lock);
}
