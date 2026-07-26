/**
 * @file network_manager_task.cpp
 * @brief 实现由单一 FreeRTOS 任务串行驱动的 Wi-Fi 与配网状态机
 */
#include "network_manager_internal.h"

#include <string.h>
#include <type_traits>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
#include "task_stack_stats.h"
#endif
#include "utils.h"

#define NETWORK_MANAGER_QUEUE_LENGTH    8U
#define NETWORK_MANAGER_TASK_STACK_SIZE 4096U
#define NETWORK_MANAGER_TASK_PRIORITY   4U
#define NETWORK_STA_CONNECT_TIMEOUT_MS  30000U
/** @brief 完整网络资源清理最长等待时间 */
#define NETWORK_MANAGER_STOP_TIMEOUT_MS 15000U

/** @brief 日志标签 */
static const char *TAG = "network_manager";

/** @brief 网络管理器内部命令 */
typedef enum
{
    NETWORK_COMMAND_BOOT = 0,
    NETWORK_COMMAND_RECONCILE_LINK,
    NETWORK_COMMAND_PORTAL_SUBMISSION,
    NETWORK_COMMAND_PORTAL_ACTIVITY,
    NETWORK_COMMAND_START_PORTAL,
    NETWORK_COMMAND_FORGET_AND_START_PORTAL,
    NETWORK_COMMAND_STOP,
} network_command_type_t;

/** @brief 网络管理器内部命令内容 */
typedef struct
{
    network_command_type_t      type;
    uint32_t                    session_id;
    connect_portal_submission_t submission;
} network_command_t;

static_assert(std::is_trivially_copyable_v<network_command_t>, "Network Manager 命令必须可按值复制");

/** @brief 网络管理任务内部生命周期 */
typedef enum
{
    NETWORK_LIFECYCLE_UNINITIALIZED = 0,
    NETWORK_LIFECYCLE_INITIALIZED,
    NETWORK_LIFECYCLE_STARTING,
    NETWORK_LIFECYCLE_RUNNING,
    NETWORK_LIFECYCLE_STOPPING,
    NETWORK_LIFECYCLE_CLEANUP_FAILED,
} network_lifecycle_t;

/** @brief 当前 STA 连接使用的配置来源 */
typedef enum
{
    NETWORK_CONNECTION_ACTIVE = 0,
    NETWORK_CONNECTION_DEFAULT,
    NETWORK_CONNECTION_CANDIDATE,
} network_connection_source_t;

/** @brief 收拢网络状态机单会话资源与可变状态 */
struct NetworkManagerRuntime
{
    QueueHandle_t                  command_queue           = nullptr;                         /**< 内部命令队列 */
    SemaphoreHandle_t              task_stopped            = nullptr;                         /**< Task 停止信号量 */
    TaskHandle_t                   manager_task            = nullptr;                         /**< 状态机 Task */
    network_lifecycle_t            lifecycle               = NETWORK_LIFECYCLE_UNINITIALIZED; /**< 生命周期 */
    uint32_t                       session_id              = 0;                               /**< 当前会话编号 */
    esp_err_t                      stop_result             = ESP_OK;                          /**< 最近停止结果 */
    TickType_t                     connect_deadline        = 0;                               /**< STA 连接截止时间 */
    TickType_t                     retry_deadline          = 0;                               /**< 重试等待截止时间 */
    uint8_t                        retry_count             = 0;                               /**< 当前重试次数 */
    network_manager_state_t        state                   = NETWORK_STATE_STOPPED;           /**< 当前业务状态 */
    network_manager_config_t       active_config           = {};    /**< 已生效或编译期默认配置 */
    network_manager_config_t       pending_config          = {};    /**< 尚未提交的候选配置 */
    network_manager_config_store_t config_store            = {};    /**< 借用的配置持久化回调 */
    bool                           has_saved_config        = false; /**< active 是否来自持久化存储 */
    bool                           has_pending_config      = false; /**< 是否存在候选配置 */
    network_connection_source_t    connection_source       = NETWORK_CONNECTION_ACTIVE;
    network_manager_status_t       working_status          = {};                           /**< 权威状态元数据 */
    connect_portal_info_t          portal_info             = {};                           /**< 当前 Portal 展示信息 */
    bool                           link_event_pending      = false;                        /**< 是否有待对账链路事件 */
    uint32_t                       pending_link_session_id = 0;                            /**< 待处理事件会话编号 */
    connect_link_event_t           pending_link_event      = {};                           /**< 最近链路事件 */
    portMUX_TYPE                   link_event_lock         = portMUX_INITIALIZER_UNLOCKED; /**< 链路事件锁 */
    portMUX_TYPE                   lifecycle_lock          = portMUX_INITIALIZER_UNLOCKED; /**< 生命周期锁 */
};

/** @brief Network Manager 进程期 Runtime */
static NetworkManagerRuntime s_runtime;

/** @brief 重连退避时间表，单位毫秒 */
static const uint32_t s_retry_delays_ms[] = { 1000U, 2000U, 4000U, 8000U, 16000U };

static void      network_manager_on_link_event(const connect_link_event_t *event, void *ctx);
static esp_err_t network_manager_on_portal_submission(const connect_portal_submission_t *submission, void *ctx);
static esp_err_t network_manager_on_portal_activity(void *ctx);

/**
 * @brief 读取当前生命周期和会话编号
 *
 * @param[out] out_session_id 会话编号，可为 nullptr
 * @return 当前生命周期
 */
static network_lifecycle_t network_manager_get_lifecycle(uint32_t *out_session_id)
{
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    const network_lifecycle_t lifecycle = s_runtime.lifecycle;
    if (out_session_id != nullptr)
    {
        *out_session_id = s_runtime.session_id;
    }
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    return lifecycle;
}

/**
 * @brief 判断指定会话是否仍允许接收异步事实
 *
 * @param[in] session_id 事件所属会话编号
 * @return true 当前会话仍在运行；false 会话已过期或正在停止
 */
static bool network_manager_session_is_running(uint32_t session_id)
{
    uint32_t current_session_id;
    return network_manager_get_lifecycle(&current_session_id) == NETWORK_LIFECYCLE_RUNNING
           && current_session_id == session_id;
}

/**
 * @brief 为当前会话重新注册 connect 回调
 *
 * connect_stop() 会终止回调借用，因此每次停止链路后再次启动 STA 或 Portal 前都要重新注册。
 */
static void network_manager_bind_connect_callbacks(void)
{
    uint32_t session_id;
    (void) network_manager_get_lifecycle(&session_id);
    connect_callbacks_t callbacks      = {};
    callbacks.on_link_event            = network_manager_on_link_event;
    callbacks.on_credentials_submitted = network_manager_on_portal_submission;
    callbacks.on_portal_activity       = network_manager_on_portal_activity;
    callbacks.ctx                      = reinterpret_cast<void *>(static_cast<uintptr_t>(session_id));
    connect_set_callbacks_borrow(&callbacks);
}

/**
 * @brief 判断网络配置读取错误是否可通过清除单个配置项恢复
 *
 * @param[in] error 网络配置读取错误码
 * @return true 表示配置项损坏或结构不兼容；false 表示底层存储故障
 */
static bool network_manager_is_recoverable_config_error(esp_err_t error)
{
    return error == ESP_ERR_INVALID_SIZE || error == ESP_ERR_INVALID_RESPONSE;
}

/**
 * @brief 判断配置字符串是否在容量内结束
 *
 * @param[in] value 字符串缓冲区
 * @param[in] capacity 缓冲区容量
 * @return true 已结束；false 未结束
 */
static bool network_manager_config_string_is_terminated(const char *value, size_t capacity)
{
    return value != nullptr && memchr(value, '\0', capacity) != nullptr;
}

/**
 * @brief 校验配置提供者返回的网络配置
 *
 * @param[in] config 待校验配置
 * @return true 配置可安全使用；false 必填字段缺失或字符串未结束
 */
static bool network_manager_config_is_valid(const network_manager_config_t *config)
{
    return config != nullptr && config->ssid[0] != '\0'
           && network_manager_config_string_is_terminated(config->ssid, sizeof(config->ssid))
           && network_manager_config_string_is_terminated(config->password, sizeof(config->password))
           && network_manager_config_string_is_terminated(config->service_url, sizeof(config->service_url))
           && network_manager_config_string_is_terminated(config->device_token, sizeof(config->device_token));
}

/**
 * @brief 取得当前 STA 尝试应使用的完整配置
 *
 * @return 借用的 active 或 pending 配置指针
 */
static const network_manager_config_t *network_manager_current_config(void)
{
    return s_runtime.connection_source == NETWORK_CONNECTION_CANDIDATE ? &s_runtime.pending_config
                                                                       : &s_runtime.active_config;
}

/**
 * @brief 发布状态元数据并通知订阅者
 *
 * @param[in] state 新状态
 * @param[in] error 对应错误码
 */
static void network_manager_publish(network_manager_state_t state, esp_err_t error)
{
    s_runtime.working_status.state      = state;
    s_runtime.working_status.last_error = error;
    s_runtime.state                     = state;
    network_manager_internal_publish_status_copy(&s_runtime.working_status, s_runtime.has_saved_config);
}

/** @brief 清除内部和公共 Portal 展示信息 */
static void network_manager_clear_portal_info(void)
{
    memset(&s_runtime.portal_info, 0, sizeof(s_runtime.portal_info));
    network_manager_internal_set_portal_info_copy(&s_runtime.portal_info);
}

/**
 * @brief 将命令投递给网络管理任务
 *
 * @param[in] command 待投递命令
 * @return ESP_OK 成功，或其他错误码
 */
static esp_err_t network_manager_post_command(const network_command_t *command)
{
    ESP_RETURN_ON_FALSE(command != nullptr, ESP_ERR_INVALID_ARG, TAG, "网络命令指针为空");
    ESP_RETURN_ON_FALSE(s_runtime.command_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "网络命令队列尚未初始化");
    uint32_t current_session_id;
    ESP_RETURN_ON_FALSE(network_manager_get_lifecycle(&current_session_id) == NETWORK_LIFECYCLE_RUNNING,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "当前没有可接收命令的网络会话");
    network_command_t queued = *command;
    if (queued.session_id == 0U)
    {
        queued.session_id = current_session_id;
    }
    ESP_RETURN_ON_FALSE(queued.session_id == current_session_id, ESP_ERR_INVALID_STATE, TAG, "网络命令所属会话已失效");
    ESP_RETURN_ON_FALSE(xQueueSend(s_runtime.command_queue, &queued, 0) == pdTRUE,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "网络命令队列已满");
    return ESP_OK;
}

/**
 * @brief 原子取走最近一个待对账的链路事件
 *
 * 多个底层事件可以合并，因为 manager 会重新查询当前物理链路，而不依赖事件历史重放。
 *
 * @param[out] out 最近链路事件
 * @return true 已取到事件；false 当前没有待处理事件
 */
static bool network_manager_take_pending_link_event(uint32_t expected_session_id, connect_link_event_t *out)
{
    if (out == nullptr)
    {
        return false;
    }
    taskENTER_CRITICAL(&s_runtime.link_event_lock);
    const bool pending     = s_runtime.link_event_pending && s_runtime.pending_link_session_id == expected_session_id;
    const bool had_pending = s_runtime.link_event_pending;
    if (had_pending)
    {
        if (pending)
        {
            *out = s_runtime.pending_link_event;
        }
        s_runtime.link_event_pending = false;
    }
    taskEXIT_CRITICAL(&s_runtime.link_event_lock);
    return pending;
}

/**
 * @brief 将 connect 链路事件复制为管理器命令
 *
 * @param[in] event connect 底层链路事件
 * @param[in] ctx 会话编号编码值
 */
static void network_manager_on_link_event(const connect_link_event_t *event, void *ctx)
{
    const uint32_t session_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    if (event == nullptr || !network_manager_session_is_running(session_id))
    {
        return;
    }
    taskENTER_CRITICAL(&s_runtime.link_event_lock);
    const bool needs_wake             = !s_runtime.link_event_pending;
    s_runtime.pending_link_event      = *event;
    s_runtime.pending_link_session_id = session_id;
    s_runtime.link_event_pending      = true;
    taskEXIT_CRITICAL(&s_runtime.link_event_lock);

    if (!needs_wake)
    {
        return;
    }
    network_command_t command = {};
    command.type              = NETWORK_COMMAND_RECONCILE_LINK;
    command.session_id        = session_id;
    if (network_manager_post_command(&command) != ESP_OK)
    {
        ESP_LOGW(TAG, "链路事件唤醒命令未入队，将在现有命令后对账");
    }
}

/**
 * @brief 将 Portal 表单提交转换为管理器命令
 *
 * @param[in] submission 用户提交的网络配置
 * @param[in] ctx 会话编号编码值
 * @return ESP_OK 命令已入队；其他值表示稍后可重试提交
 */
static esp_err_t network_manager_on_portal_submission(const connect_portal_submission_t *submission, void *ctx)
{
    const uint32_t session_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    ESP_RETURN_ON_FALSE(submission != nullptr, ESP_ERR_INVALID_ARG, TAG, "Portal 提交内容为空");
    ESP_RETURN_ON_FALSE(network_manager_session_is_running(session_id),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "Portal 提交所属会话已失效");
    network_command_t command = {};
    command.type              = NETWORK_COMMAND_PORTAL_SUBMISSION;
    command.session_id        = session_id;
    command.submission        = *submission;
    return network_manager_post_command(&command);
}

/**
 * @brief 将 Portal 显式用户活动转换为管理器命令
 *
 * @param[in] ctx 会话编号编码值
 * @return ESP_OK 命令已入队；其他值表示活动事实未接收
 */
static esp_err_t network_manager_on_portal_activity(void *ctx)
{
    const uint32_t session_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    ESP_RETURN_ON_FALSE(network_manager_session_is_running(session_id),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "Portal 活动所属会话已失效");
    network_command_t command = {};
    command.type              = NETWORK_COMMAND_PORTAL_ACTIVITY;
    command.session_id        = session_id;
    return network_manager_post_command(&command);
}

/**
 * @brief 根据当前连接来源构造 STA 配置
 *
 * @param[out] out STA 配置输出指针
 */
static void network_manager_make_sta_config(connect_sta_config_t *out)
{
    memset(out, 0, sizeof(*out));
    const network_manager_config_t *config = network_manager_current_config();
    utils_copy_string(out->ssid, sizeof(out->ssid), config->ssid);
    utils_copy_string(out->password, sizeof(out->password), config->password);
}

/**
 * @brief 发起一次 STA 连接
 */
static void network_manager_connect_station(void);

/**
 * @brief 进入配网模式
 */
static void network_manager_enter_portal(void);

/**
 * @brief 安排下一次 active 配置重连或结束本轮自动连接
 *
 * @param[in] error 本次失败错误码
 */
static void network_manager_schedule_retry(esp_err_t error)
{
    const size_t retry_limit = sizeof(s_retry_delays_ms) / sizeof(s_retry_delays_ms[0]);
    if (s_runtime.retry_count >= retry_limit)
    {
        ESP_LOGW(TAG, "本轮 Wi-Fi 连接已结束，等待 Application 决定后续策略");
        network_manager_publish(NETWORK_STATE_ERROR, error);
        return;
    }
    const uint32_t delay_ms  = s_retry_delays_ms[s_runtime.retry_count++];
    s_runtime.retry_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    network_manager_publish(NETWORK_STATE_RETRY_WAIT, error);
    ESP_LOGW(TAG,
             "Wi-Fi 连接失败，%lu 秒后进行第 %u 次连接尝试: %s",
             (unsigned long) (delay_ms / 1000U),
             (unsigned int) (s_runtime.retry_count + 1U),
             esp_err_to_name(error));
}

/** @brief 根据候选连接错误更新 Portal 状态文本 */
static void network_manager_set_candidate_error_status(esp_err_t error)
{
    switch (error)
    {
        case ESP_ERR_TIMEOUT:
            connect_set_portal_status_borrow("获取 IP 超时，请检查路由器后重试");
            break;
        default:
            connect_set_portal_status_borrow("连接失败，请返回修改配置后重试");
            break;
    }
}

/**
 * @brief 放弃候选配置并恢复等待用户输入的 Portal 状态
 *
 * @param[in] error 候选连接错误码
 * @param[in] status_message 优先展示的中文状态；NULL 时按错误码选择通用提示
 */
static void network_manager_return_candidate_to_portal(esp_err_t error, const char *status_message)
{
    const esp_err_t disconnect_error = connect_disconnect_station_keep_portal();
    if (disconnect_error != ESP_OK)
    {
        s_runtime.has_pending_config = false;
        s_runtime.connection_source  = NETWORK_CONNECTION_ACTIVE;
        memset(&s_runtime.pending_config, 0, sizeof(s_runtime.pending_config));
        network_manager_publish(NETWORK_STATE_ERROR, disconnect_error);
        return;
    }

    s_runtime.has_pending_config = false;
    s_runtime.connection_source  = NETWORK_CONNECTION_ACTIVE;
    memset(&s_runtime.pending_config, 0, sizeof(s_runtime.pending_config));
    if (status_message != nullptr)
    {
        connect_set_portal_status_borrow(status_message);
    }
    else
    {
        network_manager_set_candidate_error_status(error);
    }
    ++s_runtime.working_status.portal_activity_sequence;
    network_manager_publish(NETWORK_STATE_PROVISIONING, error);
}

/**
 * @brief 在 IPv4 可用后提交配置、关闭 Portal 并发布 ONLINE
 *
 * @param[in] link 已复核的当前物理链路
 */
static void network_manager_complete_online(const connect_link_info_t *link)
{
    if (s_runtime.connection_source == NETWORK_CONNECTION_CANDIDATE)
    {
        connect_set_portal_status_borrow("Wi-Fi 已连接，正在保存配置...");
        const esp_err_t save_error =
            s_runtime.config_store.save_config_borrow(&s_runtime.pending_config, s_runtime.config_store.ctx);
        if (save_error != ESP_OK)
        {
            network_manager_return_candidate_to_portal(save_error, "保存配置失败，请稍后重新提交");
            return;
        }
        s_runtime.active_config      = s_runtime.pending_config;
        s_runtime.has_saved_config   = true;
        s_runtime.has_pending_config = false;
        s_runtime.connection_source  = NETWORK_CONNECTION_ACTIVE;
        memset(&s_runtime.pending_config, 0, sizeof(s_runtime.pending_config));

        const esp_err_t portal_error = connect_complete_portal_station();
        if (portal_error != ESP_OK)
        {
            network_manager_publish(NETWORK_STATE_ERROR, portal_error);
            return;
        }
    }
    else if (s_runtime.connection_source == NETWORK_CONNECTION_DEFAULT)
    {
        const esp_err_t save_error =
            s_runtime.config_store.save_config_borrow(&s_runtime.active_config, s_runtime.config_store.ctx);
        if (save_error != ESP_OK)
        {
            const esp_err_t stop_error = connect_stop();
            network_manager_publish(NETWORK_STATE_ERROR, stop_error == ESP_OK ? save_error : stop_error);
            return;
        }
        s_runtime.has_saved_config = true;
    }

    s_runtime.connection_source = NETWORK_CONNECTION_ACTIVE;
    s_runtime.retry_count       = 0;
    network_manager_clear_portal_info();
    network_manager_publish(NETWORK_STATE_ONLINE, ESP_OK);
    const char *ssid = link->ssid[0] != '\0' ? link->ssid : s_runtime.active_config.ssid;
    ESP_LOGI(TAG, "Wi-Fi 已联网: SSID=%s, IP=%s, RSSI=%d dBm", ssid, link->ip, (int) link->rssi_dbm);
}

/** @brief 使用 active 或编译期默认配置发起一次 STA 连接 */
static void network_manager_connect_station(void)
{
    connect_sta_config_t config;
    network_manager_make_sta_config(&config);
    ESP_LOGI(TAG, "开始第 %u 次 Wi-Fi 连接: SSID=%s", (unsigned int) (s_runtime.retry_count + 1U), config.ssid);
    network_manager_bind_connect_callbacks();
    network_manager_clear_portal_info();
    const esp_err_t error = connect_request_start_station_copy(&config);
    if (error != ESP_OK)
    {
        const esp_err_t stop_error = connect_stop();
        network_manager_schedule_retry(stop_error == ESP_OK ? error : stop_error);
        return;
    }
    s_runtime.connect_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NETWORK_STA_CONNECT_TIMEOUT_MS);
    network_manager_publish(NETWORK_STATE_CONNECTING, ESP_OK);
}

/** @brief 进入配网模式并保留已有 active 配置 */
static void network_manager_enter_portal(void)
{
    const esp_err_t stop_error = connect_stop();
    network_manager_clear_portal_info();
    s_runtime.has_pending_config = false;
    s_runtime.connection_source  = NETWORK_CONNECTION_ACTIVE;
    memset(&s_runtime.pending_config, 0, sizeof(s_runtime.pending_config));
    if (stop_error != ESP_OK)
    {
        network_manager_publish(NETWORK_STATE_ERROR, stop_error);
        return;
    }
    network_manager_bind_connect_callbacks();
    connect_portal_info_t portal       = {};
    const esp_err_t       portal_error = connect_start_portal_copy(&portal);
    if (portal_error != ESP_OK)
    {
        network_manager_publish(NETWORK_STATE_ERROR, portal_error);
        return;
    }
    s_runtime.retry_count = 0;
    s_runtime.portal_info = portal;
    network_manager_internal_set_portal_info_copy(&s_runtime.portal_info);
    network_manager_publish(NETWORK_STATE_PROVISIONING, ESP_OK);
}

/** @brief 处理 Portal 提交并在保持 Portal 时验证候选配置 */
static void network_manager_handle_submission(const connect_portal_submission_t *submission)
{
    if (submission == nullptr || s_runtime.state != NETWORK_STATE_PROVISIONING || !s_runtime.portal_info.active)
    {
        connect_set_portal_status_borrow("当前正在处理上一份配置，请稍候");
        return;
    }

    memset(&s_runtime.pending_config, 0, sizeof(s_runtime.pending_config));
    utils_copy_string(s_runtime.pending_config.ssid, sizeof(s_runtime.pending_config.ssid), submission->ssid);
    utils_copy_string(s_runtime.pending_config.password,
                      sizeof(s_runtime.pending_config.password),
                      submission->password);
    utils_copy_string(s_runtime.pending_config.service_url,
                      sizeof(s_runtime.pending_config.service_url),
                      submission->service_url);
    utils_copy_string(s_runtime.pending_config.device_token,
                      sizeof(s_runtime.pending_config.device_token),
                      submission->device_token);
    s_runtime.has_pending_config = true;
    s_runtime.connection_source  = NETWORK_CONNECTION_CANDIDATE;
    s_runtime.retry_count        = 0;
    ++s_runtime.working_status.portal_activity_sequence;
    connect_set_portal_status_borrow("正在验证 Wi-Fi 配置...");

    connect_sta_config_t config;
    network_manager_make_sta_config(&config);
    const esp_err_t error = connect_request_start_station_with_portal_copy(&config);
    if (error != ESP_OK)
    {
        network_manager_return_candidate_to_portal(error, nullptr);
        return;
    }
    s_runtime.connect_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NETWORK_STA_CONNECT_TIMEOUT_MS);
    network_manager_publish(NETWORK_STATE_VALIDATING, ESP_OK);
}

/**
 * @brief 判断物理链路查询结果是否已具备可用 IPv4
 *
 * @param[in] link 物理链路查询结果
 * @return true 已关联 AP 且具有 IPv4；false 链路尚不可用
 */
static bool network_manager_link_is_usable(const connect_link_info_t *link)
{
    return link != nullptr && link->associated && link->has_ipv4;
}

/**
 * @brief 在 manager 单任务内解释一个 connect 原始链路事件
 *
 * 事件只作为唤醒提示；状态转换前再次查询物理链路，避免延迟事件污染新一轮连接。
 * 连接期间收到明确断开且链路仍不可用时立即结束本次尝试；30 秒截止时间仅兜底没有终态
 * 事件或已关联但尚未取得 IPv4 的情况。
 *
 * @param[in] event connect 原始链路事件
 */
static void network_manager_handle_link_event(const connect_link_event_t *event)
{
    if (event == nullptr)
    {
        return;
    }

    if (s_runtime.state != NETWORK_STATE_CONNECTING && s_runtime.state != NETWORK_STATE_VALIDATING
        && s_runtime.state != NETWORK_STATE_ONLINE)
    {
        return;
    }

    connect_link_info_t link      = {};
    const esp_err_t     query_err = connect_get_link_snapshot_copy(&link);
    if (s_runtime.state == NETWORK_STATE_CONNECTING || s_runtime.state == NETWORK_STATE_VALIDATING)
    {
        if (query_err == ESP_OK && network_manager_link_is_usable(&link))
        {
            network_manager_complete_online(&link);
        }
        else if (event->type == CONNECT_LINK_EVENT_GOT_IP)
        {
            ESP_LOGW(TAG, "忽略已失效的获取 IP 事件：%s", esp_err_to_name(query_err));
        }
        else
        {
            ESP_LOGW(TAG, "连接尝试已断开：reason=%u", static_cast<unsigned int>(event->disconnect_reason));
            const esp_err_t link_error = query_err != ESP_OK ? query_err : ESP_FAIL;
            if (s_runtime.state == NETWORK_STATE_VALIDATING)
            {
                network_manager_return_candidate_to_portal(link_error, nullptr);
                return;
            }
            const esp_err_t stop_error = connect_stop();
            network_manager_schedule_retry(stop_error == ESP_OK ? link_error : stop_error);
        }
        return;
    }

    if (query_err == ESP_OK && network_manager_link_is_usable(&link))
    {
        network_manager_publish(s_runtime.state, ESP_OK);
        return;
    }

    ESP_LOGW(TAG,
             "在线链路已失效，开始重连：reason=%u，query=%s",
             static_cast<unsigned int>(event->disconnect_reason),
             esp_err_to_name(query_err));
    const esp_err_t link_error = query_err != ESP_OK ? query_err : ESP_FAIL;
    const esp_err_t stop_error = connect_stop();
    network_manager_schedule_retry(stop_error == ESP_OK ? link_error : stop_error);
}

/**
 * @brief 获取队列等待时间，以在连接或重连截止时间到达时唤醒任务
 *
 * @return 等待 Tick 数，portMAX_DELAY 表示当前没有截止时间
 */
static TickType_t network_manager_get_wait_ticks(void)
{
    TickType_t deadline;
    if (s_runtime.state == NETWORK_STATE_CONNECTING || s_runtime.state == NETWORK_STATE_VALIDATING)
    {
        deadline = s_runtime.connect_deadline;
    }
    else if (s_runtime.state == NETWORK_STATE_RETRY_WAIT)
    {
        deadline = s_runtime.retry_deadline;
    }
    else
    {
        return portMAX_DELAY;
    }
    const TickType_t now = xTaskGetTickCount();
    if ((int32_t) (deadline - now) <= 0)
    {
        return 0;
    }
    return deadline - now;
}

/** @brief 在 Portal 活动事实到达时推进序号并重新发布当前交互状态 */
static void network_manager_handle_portal_activity(void)
{
    if (!s_runtime.portal_info.active
        || (s_runtime.state != NETWORK_STATE_PROVISIONING && s_runtime.state != NETWORK_STATE_VALIDATING))
    {
        return;
    }
    ++s_runtime.working_status.portal_activity_sequence;
    network_manager_publish(s_runtime.state, s_runtime.working_status.last_error);
}

/**
 * @brief 在管理任务内完成本轮会话清理并等待停止方回收任务
 */
static void network_manager_finish_session(void)
{
    s_runtime.retry_count = 0;
    network_manager_publish(NETWORK_STATE_STOPPING, ESP_OK);
    ESP_LOGI(TAG, "开始关闭 Wi-Fi 会话并释放网络资源");
    const esp_err_t stop_err     = connect_deinit();
    s_runtime.has_pending_config = false;
    s_runtime.connection_source  = NETWORK_CONNECTION_ACTIVE;
    memset(&s_runtime.pending_config, 0, sizeof(s_runtime.pending_config));
    network_manager_clear_portal_info();
    network_manager_publish(stop_err == ESP_OK ? NETWORK_STATE_STOPPED : NETWORK_STATE_ERROR, stop_err);
    if (stop_err == ESP_OK)
    {
        ESP_LOGI(TAG, "Wi-Fi 会话已关闭，网络资源已释放");
    }
    s_runtime.stop_result = stop_err;
    (void) xSemaphoreGive(s_runtime.task_stopped);
    vTaskSuspend(nullptr);
}

/**
 * @brief 网络管理任务入口
 *
 * @param[in] arg 未使用参数
 */
static void network_manager_task(void *arg)
{
    const uint32_t task_session_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
    task_stack_stats_t stack_stats = TASK_STACK_STATS_INITIALIZER;
#endif
    while (true)
    {
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
        task_stack_stats_log_if_due(&stack_stats, "network_manager");
#endif
        connect_link_event_t pending_event = {};
        if (network_manager_session_is_running(task_session_id)
            && network_manager_take_pending_link_event(task_session_id, &pending_event))
        {
            network_manager_handle_link_event(&pending_event);
        }

        network_command_t command;
        if (xQueueReceive(s_runtime.command_queue, &command, network_manager_get_wait_ticks()) != pdTRUE)
        {
            if (!network_manager_session_is_running(task_session_id))
            {
                continue;
            }
            if (s_runtime.state == NETWORK_STATE_RETRY_WAIT)
            {
                network_manager_connect_station();
            }
            else if (s_runtime.state == NETWORK_STATE_CONNECTING || s_runtime.state == NETWORK_STATE_VALIDATING)
            {
                connect_link_info_t link      = {};
                const esp_err_t     query_err = connect_get_link_snapshot_copy(&link);
                if (query_err == ESP_OK && network_manager_link_is_usable(&link))
                {
                    ESP_LOGW(TAG, "连接成功事件丢失，已通过链路状态恢复");
                    network_manager_complete_online(&link);
                }
                else
                {
                    ESP_LOGW(TAG, "等待 Wi-Fi 获取 IP 超时：%u 毫秒", NETWORK_STA_CONNECT_TIMEOUT_MS);
                    const esp_err_t failure_error = query_err != ESP_OK ? query_err : ESP_ERR_TIMEOUT;
                    if (s_runtime.state == NETWORK_STATE_VALIDATING)
                    {
                        network_manager_return_candidate_to_portal(failure_error, nullptr);
                    }
                    else
                    {
                        const esp_err_t stop_error = connect_stop();
                        network_manager_schedule_retry(stop_error == ESP_OK ? failure_error : stop_error);
                    }
                }
            }
            continue;
        }
        if (command.session_id != task_session_id)
        {
            continue;
        }
        if (command.type == NETWORK_COMMAND_STOP)
        {
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
            task_stack_stats_log_now("network_manager");
#endif
            network_manager_finish_session();
            return;
        }
        switch (command.type)
        {
            case NETWORK_COMMAND_BOOT: {
                memset(&s_runtime.active_config, 0, sizeof(s_runtime.active_config));
                memset(&s_runtime.pending_config, 0, sizeof(s_runtime.pending_config));
                s_runtime.has_saved_config                        = false;
                s_runtime.has_pending_config                      = false;
                s_runtime.connection_source                       = NETWORK_CONNECTION_ACTIVE;
                s_runtime.working_status.portal_activity_sequence = 0U;
                network_manager_clear_portal_info();
                esp_err_t err =
                    s_runtime.config_store.load_config_copy(&s_runtime.active_config, s_runtime.config_store.ctx);
                if (err == ESP_OK && !network_manager_config_is_valid(&s_runtime.active_config))
                {
                    err = ESP_ERR_INVALID_RESPONSE;
                }
                if (err == ESP_OK)
                {
                    s_runtime.has_saved_config  = true;
                    s_runtime.connection_source = NETWORK_CONNECTION_ACTIVE;
                    s_runtime.retry_count       = 0;
                    network_manager_connect_station();
                }
                else if (err == ESP_ERR_NOT_FOUND)
                {
                    /* 持久化存储无配置，尝试使用编译期默认值 */
                    const char *default_ssid = CONFIG_NETWORK_MANAGER_DEFAULT_SSID;
                    if (default_ssid[0] != '\0')
                    {
                        utils_copy_string(s_runtime.active_config.ssid,
                                          sizeof(s_runtime.active_config.ssid),
                                          default_ssid);
                        utils_copy_string(s_runtime.active_config.password,
                                          sizeof(s_runtime.active_config.password),
                                          CONFIG_NETWORK_MANAGER_DEFAULT_PASSWORD);
                        utils_copy_string(s_runtime.active_config.service_url,
                                          sizeof(s_runtime.active_config.service_url),
                                          CONFIG_NETWORK_MANAGER_DEFAULT_SERVICE_URL);
                        ESP_LOGI(TAG, "持久化存储无网络配置，使用编译期默认值：%s", default_ssid);
                        s_runtime.connection_source = NETWORK_CONNECTION_DEFAULT;
                        s_runtime.retry_count       = 0;
                        network_manager_connect_station();
                    }
                    else
                    {
                        network_manager_publish(NETWORK_STATE_ERROR, err);
                    }
                }
                else if (network_manager_is_recoverable_config_error(err))
                {
                    ESP_LOGW(TAG, "网络配置损坏或版本不兼容，清除该配置并重新配网：%s", esp_err_to_name(err));
                    const esp_err_t erase_err = s_runtime.config_store.erase_config(s_runtime.config_store.ctx);
                    if (erase_err != ESP_OK && erase_err != ESP_ERR_NOT_FOUND)
                    {
                        network_manager_publish(NETWORK_STATE_ERROR, erase_err);
                        break;
                    }
                    memset(&s_runtime.active_config, 0, sizeof(s_runtime.active_config));
                    network_manager_publish(NETWORK_STATE_ERROR, ESP_ERR_NOT_FOUND);
                }
                else
                {
                    network_manager_publish(NETWORK_STATE_ERROR, err);
                }
                break;
            }
            case NETWORK_COMMAND_RECONCILE_LINK:
                break;
            case NETWORK_COMMAND_PORTAL_SUBMISSION:
                network_manager_handle_submission(&command.submission);
                break;
            case NETWORK_COMMAND_PORTAL_ACTIVITY:
                network_manager_handle_portal_activity();
                break;
            case NETWORK_COMMAND_START_PORTAL:
                network_manager_enter_portal();
                break;
            case NETWORK_COMMAND_FORGET_AND_START_PORTAL: {
                const esp_err_t err = s_runtime.config_store.erase_config(s_runtime.config_store.ctx);
                if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
                {
                    network_manager_publish(NETWORK_STATE_ERROR, err);
                    break;
                }
                memset(&s_runtime.active_config, 0, sizeof(s_runtime.active_config));
                memset(&s_runtime.pending_config, 0, sizeof(s_runtime.pending_config));
                s_runtime.has_saved_config   = false;
                s_runtime.has_pending_config = false;
                s_runtime.connection_source  = NETWORK_CONNECTION_ACTIVE;
                network_manager_enter_portal();
                break;
            }
            default:
                break;
        }
    }
}

/**
 * @brief 初始化网络管理状态机任务资源
 *
 * @return ESP_OK 成功，或其他错误码
 */
esp_err_t network_manager_internal_task_init_borrow(const network_manager_config_store_t *config_store)
{
    ESP_RETURN_ON_FALSE(config_store != nullptr && config_store->load_config_copy != nullptr
                            && config_store->save_config_borrow != nullptr && config_store->erase_config != nullptr,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "网络配置持久化回调不完整");
    if (network_manager_get_lifecycle(nullptr) != NETWORK_LIFECYCLE_UNINITIALIZED)
    {
        return ESP_OK;
    }
    esp_err_t ret           = ESP_OK;
    s_runtime.config_store  = *config_store;
    s_runtime.command_queue = xQueueCreate(NETWORK_MANAGER_QUEUE_LENGTH, sizeof(network_command_t));
    ESP_GOTO_ON_FALSE(s_runtime.command_queue != nullptr, ESP_ERR_NO_MEM, cleanup, TAG, "创建网络管理命令队列失败");
    s_runtime.task_stopped = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(s_runtime.task_stopped != nullptr,
                      ESP_ERR_NO_MEM,
                      cleanup,
                      TAG,
                      "创建网络管理任务退出信号量失败");
    memset(&s_runtime.working_status, 0, sizeof(s_runtime.working_status));
    memset(&s_runtime.portal_info, 0, sizeof(s_runtime.portal_info));
    s_runtime.working_status.state = NETWORK_STATE_STOPPED;
    s_runtime.state                = NETWORK_STATE_STOPPED;
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.lifecycle = NETWORK_LIFECYCLE_INITIALIZED;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    network_manager_internal_set_portal_info_copy(&s_runtime.portal_info);
    network_manager_internal_publish_status_copy(&s_runtime.working_status, s_runtime.has_saved_config);
    return ESP_OK;

cleanup:
    if (s_runtime.task_stopped != nullptr)
    {
        vSemaphoreDelete(s_runtime.task_stopped);
        s_runtime.task_stopped = nullptr;
    }
    if (s_runtime.command_queue != nullptr)
    {
        vQueueDelete(s_runtime.command_queue);
        s_runtime.command_queue = nullptr;
    }
    memset(&s_runtime.config_store, 0, sizeof(s_runtime.config_store));
    return ret;
}

/**
 * @brief 回滚未完成的网络会话启动
 *
 * @param[in] error 触发回滚的错误码
 * @return 原始错误码；若清理失败则返回清理错误码
 */
static esp_err_t network_manager_rollback_start(esp_err_t error)
{
    connect_set_callbacks_borrow(nullptr);
    const esp_err_t cleanup_err = connect_deinit();
    if (cleanup_err != ESP_OK)
    {
        ESP_LOGE(TAG, "网络会话启动回滚清理失败：%s", esp_err_to_name(cleanup_err));
        error = cleanup_err;
    }
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.manager_task = nullptr;
    s_runtime.lifecycle    = cleanup_err == ESP_OK ? NETWORK_LIFECYCLE_INITIALIZED : NETWORK_LIFECYCLE_CLEANUP_FAILED;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    return error;
}

/**
 * @brief 启动网络管理状态机任务
 *
 * @return ESP_OK 成功，或其他错误码
 */
esp_err_t network_manager_internal_task_start(void)
{
    uint32_t session_id = 0U;
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    const bool can_start = s_runtime.lifecycle == NETWORK_LIFECYCLE_INITIALIZED;
    if (can_start)
    {
        s_runtime.lifecycle = NETWORK_LIFECYCLE_STARTING;
        ++s_runtime.session_id;
        if (s_runtime.session_id == 0U)
        {
            ++s_runtime.session_id;
        }
        session_id = s_runtime.session_id;
    }
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    ESP_RETURN_ON_FALSE(can_start, ESP_ERR_INVALID_STATE, TAG, "网络管理任务当前状态不允许启动");

    (void) xQueueReset(s_runtime.command_queue);
    (void) xSemaphoreTake(s_runtime.task_stopped, 0);
    taskENTER_CRITICAL(&s_runtime.link_event_lock);
    s_runtime.link_event_pending = false;
    taskEXIT_CRITICAL(&s_runtime.link_event_lock);

    esp_err_t err = connect_init();
    if (err != ESP_OK)
    {
        return network_manager_rollback_start(err);
    }
    network_manager_bind_connect_callbacks();

    TaskHandle_t task = nullptr;
    if (xTaskCreate(network_manager_task,
                    "network_manager",
                    NETWORK_MANAGER_TASK_STACK_SIZE,
                    reinterpret_cast<void *>(static_cast<uintptr_t>(session_id)),
                    NETWORK_MANAGER_TASK_PRIORITY,
                    &task)
        != pdPASS)
    {
        return network_manager_rollback_start(ESP_ERR_NO_MEM);
    }

    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.manager_task = task;
    s_runtime.lifecycle    = NETWORK_LIFECYCLE_RUNNING;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);

    network_command_t command = {};
    command.type              = NETWORK_COMMAND_BOOT;
    command.session_id        = session_id;
    err                       = network_manager_post_command(&command);
    if (err != ESP_OK)
    {
        taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
        s_runtime.lifecycle = NETWORK_LIFECYCLE_STOPPING;
        taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
        vTaskDelete(task);
        s_runtime.manager_task = nullptr;
        return network_manager_rollback_start(err);
    }
    return ESP_OK;
}

/**
 * @brief 同步停止网络管理状态机任务和底层 Wi-Fi 会话
 *
 * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE 生命周期不允许；其他值表示清理失败
 */
esp_err_t network_manager_internal_task_stop(void)
{
    uint32_t     session_id = 0U;
    TaskHandle_t task       = nullptr;
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    const bool can_stop =
        (s_runtime.lifecycle == NETWORK_LIFECYCLE_RUNNING || s_runtime.lifecycle == NETWORK_LIFECYCLE_STOPPING)
        && s_runtime.manager_task != nullptr && s_runtime.manager_task != xTaskGetCurrentTaskHandle();
    if (can_stop)
    {
        s_runtime.lifecycle = NETWORK_LIFECYCLE_STOPPING;
        session_id          = s_runtime.session_id;
        task                = s_runtime.manager_task;
    }
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    ESP_RETURN_ON_FALSE(can_stop, ESP_ERR_INVALID_STATE, TAG, "网络管理任务当前状态不允许停止");

    /* STOPPING 后所有生产者都会拒绝新命令；丢弃旧业务命令以保证停止命令必有槽位。 */
    (void) xQueueReset(s_runtime.command_queue);
    network_command_t command = {};
    command.type              = NETWORK_COMMAND_STOP;
    command.session_id        = session_id;
    ESP_RETURN_ON_FALSE(xQueueSendToFront(s_runtime.command_queue, &command, 0U) == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "停止命令入队失败，保持 STOPPING 并等待后续确认");

    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_runtime.task_stopped, pdMS_TO_TICKS(NETWORK_MANAGER_STOP_TIMEOUT_MS))
                            == pdTRUE,
                        ESP_ERR_TIMEOUT,
                        TAG,
                        "等待网络管理任务安全清理超时，保持 STOPPING 并禁止新会话");
    vTaskDelete(task);
    (void) xQueueReset(s_runtime.command_queue);
    taskENTER_CRITICAL(&s_runtime.link_event_lock);
    s_runtime.link_event_pending = false;
    taskEXIT_CRITICAL(&s_runtime.link_event_lock);

    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.manager_task = nullptr;
    s_runtime.lifecycle =
        s_runtime.stop_result == ESP_OK ? NETWORK_LIFECYCLE_INITIALIZED : NETWORK_LIFECYCLE_CLEANUP_FAILED;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    ESP_RETURN_ON_ERROR(s_runtime.stop_result, TAG, "网络会话底层资源清理失败");
    return ESP_OK;
}

/**
 * @brief 判断 Network Manager 是否仍持有活动或待清理 Task
 *
 * @return true Task 仍存在；false 已完成回收
 */
bool network_manager_internal_task_has_active_task(void)
{
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    const bool active = s_runtime.manager_task != nullptr;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    return active;
}

/**
 * @brief 请求进入配网模式
 *
 * @return ESP_OK 命令已入队；其他值表示管理器未就绪或队列已满
 */
esp_err_t network_manager_internal_request_start_portal(void)
{
    network_command_t command = {};
    command.type              = NETWORK_COMMAND_START_PORTAL;
    return network_manager_post_command(&command);
}

/**
 * @brief 清除网络配置并进入配网模式
 *
 * @return ESP_OK 命令已入队；其他值表示管理器未就绪或队列已满
 */
esp_err_t network_manager_internal_request_forget_and_start_portal(void)
{
    network_command_t command = {};
    command.type              = NETWORK_COMMAND_FORGET_AND_START_PORTAL;
    return network_manager_post_command(&command);
}
