/*
 * 文件职责：拥有 DeskMate 网络产品命令、Manager 变化收敛、Dashboard/OTA 调度和唯一网络 Application Task。
 */
#include "app_network.h"

#include <string.h>

#include "calendar.h"
#include "dashboard_store.h"
#include "deskmate_api.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "firmware_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mail.h"
#include "network_manager.h"
#include "ota_presenter.h"
#include "presentation_dispatch.h"
#include "quota.h"
#include "remote_log.h"
#include "sdkconfig.h"
#include "settings_store.h"
#include "status_bar_presenter.h"
#include "system_clock.h"
#include "system_info.h"
#include "task_stack_stats.h"
#include "weather.h"

#define NETWORK_TASK_STACK                 8192U
#define NETWORK_TASK_PRIORITY              2U
#define NETWORK_COMMAND_QUEUE_LENGTH       20U
#define NETWORK_CONNECT_TIMEOUT_MS         CONFIG_DESKMATE_API_HTTP_TIMEOUT_MS
#define NETWORK_CONTROL_RESPONSE_SLOTS     2U
#define NETWORK_MANAGER_POLL_INTERVAL_MS   100U
#define NETWORK_TIME_SYNC_TIMEOUT_MS       10000U
#define REMOTE_LOG_STOP_TIMEOUT_MS         5000U
#define DESKMATE_REMOTE_LOG_PRODUCT_ID     2U
#define DESKMATE_REMOTE_LOG_QUEUE_CAPACITY 8U
#define DESKMATE_REMOTE_LOG_BATCH_CAPACITY 4U

typedef enum
{
    NETWORK_COMMAND_START_SESSION = 0,
    NETWORK_COMMAND_RESTART_SESSION,
    NETWORK_COMMAND_MANAGER_CHANGED,
    NETWORK_COMMAND_START_PORTAL,
    NETWORK_COMMAND_SYNC,
    NETWORK_COMMAND_MAINTENANCE_SYNC,
    NETWORK_COMMAND_OTA_CHECK,
    NETWORK_COMMAND_OTA_INSTALL,
    NETWORK_COMMAND_OTA_EVENT,
    NETWORK_COMMAND_SYNC_TIME,
    NETWORK_COMMAND_LEASE_ACQUIRE,
    NETWORK_COMMAND_LEASE_RELEASE,
    NETWORK_COMMAND_SUSPEND_FOR_LIGHT_SLEEP,
    NETWORK_COMMAND_RESUME_FROM_LIGHT_SLEEP,
} network_command_type_t;

typedef struct
{
    network_command_type_t       type;
    app_network_ota_check_mode_t ota_check_mode;
    app_network_lease_type_t     lease_type;
    uint8_t                      response_slot;
    uint32_t                     request_id;
    uint32_t                     lease_generation;
    int64_t                      deadline_us;
    firmware_ota_event_t         ota_event;
} network_command_t;

typedef enum
{
    CONTROL_RESPONSE_FREE = 0,
    CONTROL_RESPONSE_PENDING,
    CONTROL_RESPONSE_EXECUTING,
    CONTROL_RESPONSE_COMPLETED,
} control_response_state_t;

typedef struct
{
    StaticSemaphore_t        signal_storage;
    SemaphoreHandle_t        signal;
    control_response_state_t state;
    uint32_t                 request_id;
    esp_err_t                result;
    uint32_t                 generation;
} control_response_slot_t;

static const char *TAG = "app_network_task";

static QueueHandle_t                      s_command_queue;
static TaskHandle_t                       s_task;
static esp_timer_handle_t                 s_periodic_timer;
static esp_timer_handle_t                 s_reconnect_timer;
static esp_timer_handle_t                 s_ota_timer;
static esp_timer_handle_t                 s_time_sync_timer;
static portMUX_TYPE                       s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool                               s_sync_queued;
static bool                               s_sync_running;
static bool                               s_sync_cancel_requested;
static bool                               s_periodic_enabled;
static bool                               s_ota_queued;
static bool                               s_ota_running;
static bool                               s_ota_pending_update;
static app_network_ota_check_mode_t       s_ota_check_mode = APP_NETWORK_OTA_CHECK_MANUAL;
static bool                               s_ota_auto_check_enabled;
static app_network_lease_type_t           s_active_lease_type = APP_NETWORK_LEASE_NONE;
static bool                               s_portal_transition_pending;
static uint32_t                           s_portal_transition_activity_sequence;
static bool                               s_light_sleep_suspended;
static bool                               s_manager_change_pending;
static bool                               s_dashboard_online;
static int64_t                            s_next_refresh_at_utc;
static bool                               s_remote_log_initialized;
static uint32_t                           s_active_lease_generation;
static uint32_t                           s_next_lease_generation = 1U;
static uint32_t                           s_next_control_request_id;
static uint32_t                           s_reconnect_backoff_ms;
static network_manager_state_t            s_last_manager_state = NETWORK_STATE_STOPPED;
static control_response_slot_t            s_control_slots[NETWORK_CONTROL_RESPONSE_SLOTS];
static app_network_link_change_callback_t s_link_change_callback;
static void                              *s_link_change_callback_context;

static void on_network_manager_notify(void *ctx);

/**
 * @brief 以 DeskMate 的内存预算初始化远端日志捕获
 *
 * 远端日志仅用于诊断，不能抢占轻睡眠等产品 Task 的内存。网络上线且服务配置有效时才分配缓存，
 * 并保留较小的队列与上传批次；日志突发时允许组件按自身策略丢弃日志，主产品功能不受影响。
 *
 * @return ESP_OK 初始化成功；其他值表示本轮仅保留本地串口日志
 */
static esp_err_t initialize_remote_log_capture(void)
{
    if (s_remote_log_initialized)
    {
        return ESP_OK;
    }

    remote_log_config_t config;
    ESP_RETURN_ON_ERROR(remote_log_config_set_defaults(&config), TAG, "加载远端日志默认配置失败");
    config.queue_capacity = DESKMATE_REMOTE_LOG_QUEUE_CAPACITY;
    config.batch_capacity = DESKMATE_REMOTE_LOG_BATCH_CAPACITY;

    const esp_err_t error = remote_log_init(&config);
    if (error == ESP_OK)
    {
        s_remote_log_initialized = true;
        ESP_LOGI(TAG,
                 "远端日志捕获已按低内存配置初始化: 队列=%u，批次=%u",
                 (unsigned) config.queue_capacity,
                 (unsigned) config.batch_capacity);
    }
    return error;
}

/**
 * @brief 按当前服务配置尽力启动 DeskMate 远端日志上传
 *
 * 调用方必须已确认 Network Manager ONLINE；本函数不改变网络会话、重试或轻睡眠策略。配置或
 * 启动失败仅保留本地串口日志，不影响 Dashboard 等产品能力。
 */
static void start_remote_log_upload(void)
{
    device_settings_t settings;
    esp_err_t         error = settings_store_load_copy(&settings);
    if (error != ESP_OK || settings.service_url[0] == '\0')
    {
        ESP_LOGW(TAG,
                 "缺少远端日志服务配置，本轮只保留本地串口日志: %s",
                 esp_err_to_name(error == ESP_OK ? ESP_ERR_NOT_FOUND : error));
        return;
    }

    char device_id[DESKMATE_API_DEVICE_ID_MAX] = { 0 };
    error                                      = system_info_get_device_id(device_id, sizeof(device_id));
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "读取远端日志设备标识失败，本轮继续使用本地串口日志: %s", esp_err_to_name(error));
        return;
    }

    error = initialize_remote_log_capture();
    if (error == ESP_OK)
    {
        error = remote_log_configure_copy(settings.service_url, DESKMATE_REMOTE_LOG_PRODUCT_ID, device_id);
    }
    if (error == ESP_OK)
    {
        error = remote_log_start();
    }
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "启动远端日志上传失败，本轮继续使用串口日志: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG,
             "远端日志上传 Task 已启动: product_id=%u, device_id=%s",
             (unsigned) DESKMATE_REMOTE_LOG_PRODUCT_ID,
             device_id);
}

/**
 * @brief 在停止 Network Manager 前同步停止远端日志上传 Task
 *
 * @return ESP_OK 上传 Task 已停止或未运行；其他值表示尚不能安全停止网络会话
 */
static esp_err_t stop_remote_log_upload(void)
{
    if (!s_remote_log_initialized)
    {
        return ESP_OK;
    }
    const esp_err_t error = remote_log_stop(REMOTE_LOG_STOP_TIMEOUT_MS);
    return error == ESP_ERR_INVALID_STATE ? ESP_OK : error;
}

/** @brief 尝试投递一个按值复制的网络产品命令 */
static esp_err_t post_control_command(const network_command_t *command)
{
    if (s_command_queue == NULL || command == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_command_queue, command, 0U) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

/** @brief 在线程安全上下文读取轻睡眠暂停状态 */
static bool light_sleep_is_suspended(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool suspended = s_light_sleep_suspended;
    taskEXIT_CRITICAL(&s_state_lock);
    return suspended;
}

/**
 * @brief 把 DeskMate 持久化设置转换为 Network Manager 完整配置
 *
 * @param[out] out_config 配置副本
 * @param[in] ctx 未使用上下文
 * @return ESP_OK 已加载；ESP_ERR_NOT_FOUND 尚未配置 Wi-Fi；或存储错误码
 */
static esp_err_t load_network_config(network_manager_config_t *out_config, void *ctx)
{
    (void) ctx;
    if (out_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    device_settings_t settings;
    const esp_err_t   error = settings_store_load_copy(&settings);
    if (error != ESP_OK)
    {
        return error;
    }
    if (!settings_store_is_network_ready(&settings))
    {
        return ESP_ERR_NOT_FOUND;
    }

    memset(out_config, 0, sizeof(*out_config));
    settings_store_copy_string(out_config->ssid, sizeof(out_config->ssid), settings.wifi_ssid);
    settings_store_copy_string(out_config->password, sizeof(out_config->password), settings.wifi_password);
    settings_store_copy_string(out_config->service_url, sizeof(out_config->service_url), settings.service_url);
    settings_store_copy_string(out_config->device_token, sizeof(out_config->device_token), settings.device_token);
    return ESP_OK;
}

/**
 * @brief 保存 Network Manager 已验证的完整配置，并保留未由 Portal 覆盖的产品字段
 *
 * @param[in] config 已验证配置
 * @param[in] ctx 未使用上下文
 * @return ESP_OK 已保存；或存储错误码
 */
static esp_err_t save_network_config(const network_manager_config_t *config, void *ctx)
{
    (void) ctx;
    if (config == NULL || config->ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    device_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_store_load_copy(&settings), TAG, "读取待更新网络设置失败");
    settings_store_copy_string(settings.wifi_ssid, sizeof(settings.wifi_ssid), config->ssid);
    settings_store_copy_string(settings.wifi_password, sizeof(settings.wifi_password), config->password);
    if (config->service_url[0] != '\0')
    {
        settings_store_copy_string(settings.service_url, sizeof(settings.service_url), config->service_url);
    }
    if (config->device_token[0] != '\0')
    {
        settings_store_copy_string(settings.device_token, sizeof(settings.device_token), config->device_token);
    }
    return settings_store_save(&settings);
}

/**
 * @brief 清除 Network Manager 的 Wi-Fi 身份，同时保留 DeskMate 非网络产品设置
 *
 * @param[in] ctx 未使用上下文
 * @return ESP_OK 已清除；或存储错误码
 */
static esp_err_t erase_network_config(void *ctx)
{
    (void) ctx;
    device_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_store_load_copy(&settings), TAG, "读取待清除网络设置失败");
    settings.wifi_ssid[0]     = '\0';
    settings.wifi_password[0] = '\0';
    settings.device_token[0]  = '\0';
    return settings_store_save(&settings);
}

/** @brief 初始化固定数量的同步控制命令回执槽 */
static esp_err_t initialize_control_response_slots(void)
{
    for (size_t index = 0; index < NETWORK_CONTROL_RESPONSE_SLOTS; ++index)
    {
        control_response_slot_t *slot = &s_control_slots[index];
        if (slot->signal == NULL)
        {
            slot->signal = xSemaphoreCreateBinaryStatic(&slot->signal_storage);
            if (slot->signal == NULL)
            {
                return ESP_ERR_NO_MEM;
            }
        }
        slot->state      = CONTROL_RESPONSE_FREE;
        slot->request_id = 0U;
        slot->result     = ESP_ERR_INVALID_STATE;
        slot->generation = 0U;
        (void) xSemaphoreTake(slot->signal, 0U);
    }
    return ESP_OK;
}

/** @brief 分配一个仅以内部索引标识的控制命令回执槽 */
static esp_err_t allocate_control_response_slot(uint8_t *out_slot, uint32_t *out_request_id)
{
    taskENTER_CRITICAL(&s_state_lock);
    for (uint8_t index = 0U; index < NETWORK_CONTROL_RESPONSE_SLOTS; ++index)
    {
        control_response_slot_t *slot = &s_control_slots[index];
        if (slot->state != CONTROL_RESPONSE_FREE)
        {
            continue;
        }

        ++s_next_control_request_id;
        if (s_next_control_request_id == 0U)
        {
            ++s_next_control_request_id;
        }
        slot->state      = CONTROL_RESPONSE_PENDING;
        slot->request_id = s_next_control_request_id;
        slot->result     = ESP_ERR_INVALID_STATE;
        slot->generation = 0U;
        *out_slot        = index;
        *out_request_id  = slot->request_id;
        taskEXIT_CRITICAL(&s_state_lock);
        (void) xSemaphoreTake(slot->signal, 0U);
        return ESP_OK;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_ERR_NO_MEM;
}

/** @brief 释放尚未成功投递命令所占用的控制回执槽 */
static void release_unposted_control_response_slot(uint8_t slot_index, uint32_t request_id)
{
    if (slot_index >= NETWORK_CONTROL_RESPONSE_SLOTS)
    {
        return;
    }
    taskENTER_CRITICAL(&s_state_lock);
    control_response_slot_t *slot = &s_control_slots[slot_index];
    if (slot->request_id == request_id && slot->state == CONTROL_RESPONSE_PENDING)
    {
        slot->state = CONTROL_RESPONSE_FREE;
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

/**
 * @brief 在单个状态锁临界区仲裁控制回执完成、执行中、超时与所有权丢失
 *
 * COMPLETED 直接复制结果并释放槽；EXECUTING 已取得请求所有权，必须解锁后等待信号再重读；
 * 到期 PENDING 在同一临界区直接释放，handler 此后不能再认领。其他状态表示当前 waiter
 * 已失去该槽所有权。
 *
 * @param[in] slot_index 回执槽索引
 * @param[in] request_id 请求代次
 * @param[in] deadline_us 调用方等待截止时间
 * @param[out] out_generation 可选租约代次输出
 * @return 控制命令最终结果；ESP_ERR_TIMEOUT 未在截止时间前认领；
 *         ESP_ERR_INVALID_STATE 回执槽所有权已丢失
 */
static esp_err_t wait_control_response(uint8_t slot_index, uint32_t request_id, int64_t deadline_us,
                                       uint32_t *out_generation)
{
    if (slot_index >= NETWORK_CONTROL_RESPONSE_SLOTS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    control_response_slot_t *slot = &s_control_slots[slot_index];
    for (;;)
    {
        const int64_t now_us = esp_timer_get_time();
        TickType_t    wait_ticks;

        taskENTER_CRITICAL(&s_state_lock);
        if (slot->request_id != request_id)
        {
            taskEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (slot->state == CONTROL_RESPONSE_COMPLETED)
        {
            const esp_err_t result     = slot->result;
            const uint32_t  generation = slot->generation;
            slot->state                = CONTROL_RESPONSE_FREE;
            taskEXIT_CRITICAL(&s_state_lock);
            if (out_generation != NULL)
            {
                *out_generation = generation;
            }
            return result;
        }
        if (slot->state == CONTROL_RESPONSE_EXECUTING)
        {
            taskEXIT_CRITICAL(&s_state_lock);
            (void) xSemaphoreTake(slot->signal, portMAX_DELAY);
            continue;
        }
        if (slot->state != CONTROL_RESPONSE_PENDING)
        {
            taskEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_INVALID_STATE;
        }

        const int64_t remaining_us = deadline_us - now_us;
        if (remaining_us <= 0)
        {
            slot->state = CONTROL_RESPONSE_FREE;
            taskEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_TIMEOUT;
        }
        wait_ticks = pdMS_TO_TICKS((remaining_us + 999LL) / 1000LL);
        wait_ticks = wait_ticks > 0U ? wait_ticks : 1U;
        taskEXIT_CRITICAL(&s_state_lock);
        (void) xSemaphoreTake(slot->signal, wait_ticks);
    }
}

/** @brief 在状态锁内完成一个仍有效的控制命令回执槽 */
static SemaphoreHandle_t complete_control_response_locked(uint8_t slot_index, uint32_t request_id, esp_err_t result,
                                                          uint32_t generation)
{
    if (slot_index >= NETWORK_CONTROL_RESPONSE_SLOTS)
    {
        return NULL;
    }
    control_response_slot_t *slot = &s_control_slots[slot_index];
    if (slot->request_id != request_id
        || (slot->state != CONTROL_RESPONSE_PENDING && slot->state != CONTROL_RESPONSE_EXECUTING))
    {
        return NULL;
    }
    slot->result     = result;
    slot->generation = generation;
    slot->state      = CONTROL_RESPONSE_COMPLETED;
    return slot->signal;
}

/** @brief 完成控制命令回执并在退出状态锁后唤醒等待者 */
static void complete_control_response(const network_command_t *command, esp_err_t result)
{
    SemaphoreHandle_t signal;
    taskENTER_CRITICAL(&s_state_lock);
    signal = complete_control_response_locked(command->response_slot, command->request_id, result, 0U);
    taskEXIT_CRITICAL(&s_state_lock);
    if (signal != NULL)
    {
        (void) xSemaphoreGive(signal);
    }
}

/** @brief 原子认领一个仍在等待且未过期的同步控制命令 */
static bool begin_control_command(const network_command_t *command)
{
    const int64_t now_us  = esp_timer_get_time();
    bool          claimed = false;
    taskENTER_CRITICAL(&s_state_lock);
    if (command->response_slot < NETWORK_CONTROL_RESPONSE_SLOTS)
    {
        control_response_slot_t *slot = &s_control_slots[command->response_slot];
        if (slot->request_id == command->request_id && slot->state == CONTROL_RESPONSE_PENDING
            && now_us < command->deadline_us)
        {
            slot->state = CONTROL_RESPONSE_EXECUTING;
            claimed     = true;
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return claimed;
}

/** @brief 在线程安全上下文读取 Dashboard 同步取消标志 */
static bool is_sync_cancel_requested(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool cancelled = s_sync_cancel_requested;
    taskEXIT_CRITICAL(&s_state_lock);
    return cancelled;
}

/** @brief 读取合法 Dashboard 刷新周期并提供产品默认值 */
static uint32_t get_dashboard_refresh_seconds(void)
{
    device_settings_t settings;
    if (settings_store_load_copy(&settings) == ESP_OK && settings.refresh_seconds >= 60U
        && settings.refresh_seconds <= 3600U)
    {
        return settings.refresh_seconds;
    }
    return CONFIG_DESKMATE_API_DEFAULT_REFRESH_SEC;
}

/** @brief 仅在服务连通事实变化时更新状态栏 */
static void set_dashboard_online(bool online)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool changed = s_dashboard_online != online;
    s_dashboard_online = online;
    taskEXIT_CRITICAL(&s_state_lock);
    if (changed)
    {
        status_bar_presenter_set_server_online(online);
    }
}

/**
 * @brief 从持久化设置和稳定设备 ID 构造 DeskMate API 客户端
 *
 * @param[out] stored 本轮调用期借用的设置快照
 * @param[out] device_id 本轮调用期借用的设备 ID 缓冲区
 * @param[in] device_id_capacity 设备 ID 缓冲区容量
 * @param[out] client DeskMate API 客户端
 * @return ESP_OK 成功；其他值表示设置或设备身份读取失败
 */
static esp_err_t make_deskmate_client(device_settings_t *stored, char *device_id, size_t device_id_capacity,
                                      deskmate_api_client_t *client)
{
    ESP_RETURN_ON_ERROR(settings_store_load_copy(stored), TAG, "读取服务配置失败");
    ESP_RETURN_ON_FALSE(stored->service_url[0] != '\0', ESP_ERR_INVALID_STATE, TAG, "服务地址为空");
    ESP_RETURN_ON_ERROR(system_info_get_device_id(device_id, device_id_capacity), TAG, "生成设备 ID 失败");
    *client = (deskmate_api_client_t) {
        .base_url     = stored->service_url,
        .device_token = stored->device_token,
        .device_id    = device_id,
        .timeout_ms   = CONFIG_DESKMATE_API_HTTP_TIMEOUT_MS,
    };
    return ESP_OK;
}

/** @brief 使用本地稳定身份拉取并存储 Dashboard */
static esp_err_t fetch_dashboard(void)
{
    device_settings_t     stored;
    char                  device_id[DESKMATE_API_DEVICE_ID_MAX] = { 0 };
    deskmate_api_client_t client;
    ESP_RETURN_ON_ERROR(make_deskmate_client(&stored, device_id, sizeof(device_id), &client),
                        TAG,
                        "创建 Dashboard 客户端失败");

    deskmate_api_dashboard_t dashboard   = { 0 };
    int                      http_status = 0;
    esp_err_t                error =
        deskmate_api_get_dashboard(&client, CONFIG_DESKMATE_API_DASHBOARD_JSON_MAX, &dashboard, &http_status);
    if (http_status == 401)
    {
        ESP_LOGE(TAG, "Dashboard 鉴权失败，保留当前设备令牌并等待配置修正");
    }
    if (error != ESP_OK)
    {
        deskmate_api_dashboard_release(&dashboard);
        return error;
    }

    error = dashboard_store_update_from_json(dashboard.raw_json);
    if (error == ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_next_refresh_at_utc = dashboard.next_refresh_at_utc;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    deskmate_api_dashboard_release(&dashboard);
    return error;
}

/** @brief 按固定顺序把 Dashboard Store 刷新到四个领域快照 */
static void refresh_dashboard_services(void)
{
    esp_err_t error = weather_refresh_from_dashboard();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "刷新天气快照失败: %s", esp_err_to_name(error));
    }
    error = calendar_refresh_from_dashboard();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "刷新日历快照失败: %s", esp_err_to_name(error));
    }
    error = mail_refresh_from_dashboard();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "刷新邮件快照失败: %s", esp_err_to_name(error));
    }
    error = quota_refresh_from_dashboard();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "刷新限额快照失败: %s", esp_err_to_name(error));
    }
}

/**
 * @brief 在有限时间内等待 Network Manager 发布 ONLINE
 *
 * @param[in] timeout_ms 最长等待时间
 * @param[in] cancelable 是否响应 Dashboard 同步取消
 * @return ESP_OK 已在线；或当前网络状态对应错误码
 */
static esp_err_t ensure_network_online(uint32_t timeout_ms, bool cancelable)
{
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    for (;;)
    {
        if (cancelable && is_sync_cancel_requested())
        {
            return ESP_ERR_INVALID_STATE;
        }

        network_manager_status_t status = { 0 };
        ESP_RETURN_ON_ERROR(network_manager_get_status_copy(&status), TAG, "读取网络管理状态失败");
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

        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0)
        {
            return ESP_ERR_TIMEOUT;
        }
        uint32_t delay_ms = NETWORK_MANAGER_POLL_INTERVAL_MS;
        if (remaining_us < (int64_t) delay_ms * 1000LL)
        {
            delay_ms = (uint32_t) ((remaining_us + 999LL) / 1000LL);
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 0U ? delay_ms : 1U));
    }
}

/** @brief 停止重连 Timer 并清空 Application 会话退避状态 */
static void reset_reconnect_policy(void)
{
    if (s_reconnect_timer != NULL)
    {
        (void) esp_timer_stop(s_reconnect_timer);
    }
    s_reconnect_backoff_ms = 0U;
}

/** @brief 在 Network Manager 完成本轮内部重试后安排新会话 */
static void schedule_reconnect_backoff(void)
{
    if (s_reconnect_timer == NULL || light_sleep_is_suspended())
    {
        return;
    }
    if (s_reconnect_backoff_ms < CONFIG_DESKMATE_WIFI_RECONNECT_BACKOFF_MIN_MS)
    {
        s_reconnect_backoff_ms = CONFIG_DESKMATE_WIFI_RECONNECT_BACKOFF_MIN_MS;
    }

    ESP_LOGW(TAG, "网络会话将在 %lu ms 后重新启动", (unsigned long) s_reconnect_backoff_ms);
    (void) esp_timer_stop(s_reconnect_timer);
    const esp_err_t error = esp_timer_start_once(s_reconnect_timer, (uint64_t) s_reconnect_backoff_ms * 1000ULL);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "启动网络会话退避定时器失败: %s", esp_err_to_name(error));
        return;
    }

    if (s_reconnect_backoff_ms < CONFIG_DESKMATE_WIFI_RECONNECT_BACKOFF_MAX_MS)
    {
        uint64_t next = (uint64_t) s_reconnect_backoff_ms * 2ULL;
        if (next > CONFIG_DESKMATE_WIFI_RECONNECT_BACKOFF_MAX_MS)
        {
            next = CONFIG_DESKMATE_WIFI_RECONNECT_BACKOFF_MAX_MS;
        }
        s_reconnect_backoff_ms = (uint32_t) next;
    }
}

/** @brief 根据当前产品开关重新设置 Dashboard 周期同步 Timer */
static void reschedule_periodic_timer(void)
{
    if (s_periodic_timer == NULL)
    {
        return;
    }
    (void) esp_timer_stop(s_periodic_timer);

    taskENTER_CRITICAL(&s_state_lock);
    const bool enabled =
        s_periodic_enabled && s_active_lease_type == APP_NETWORK_LEASE_NONE && !s_light_sleep_suspended;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!enabled)
    {
        return;
    }

    const uint64_t  interval_us = (uint64_t) get_dashboard_refresh_seconds() * 1000000ULL;
    const esp_err_t error       = esp_timer_start_periodic(s_periodic_timer, interval_us);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "启动 Dashboard 周期定时器失败: %s", esp_err_to_name(error));
    }
}

/** @brief 根据 OTA 产品开关和持久化设置重新设置自动检查 Timer */
static void reschedule_ota_timer(void)
{
    if (s_ota_timer == NULL)
    {
        return;
    }
    (void) esp_timer_stop(s_ota_timer);

    taskENTER_CRITICAL(&s_state_lock);
    const bool enabled = s_ota_auto_check_enabled && s_active_lease_type == APP_NETWORK_LEASE_NONE
                         && !s_light_sleep_suspended && !s_ota_queued && !s_ota_running && !s_ota_pending_update;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!enabled)
    {
        return;
    }

    device_settings_t settings;
    const esp_err_t   settings_error = settings_store_load_copy(&settings);
    if (settings_error != ESP_OK)
    {
        ESP_LOGW(TAG, "读取 OTA 定时设置失败: %s", esp_err_to_name(settings_error));
        return;
    }
    if (!settings.ota_enabled || settings.ota_check_interval_sec == 0U)
    {
        return;
    }

    const uint64_t  interval_us = (uint64_t) settings.ota_check_interval_sec * 1000000ULL;
    const esp_err_t error       = esp_timer_start_periodic(s_ota_timer, interval_us);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "启动 OTA 自动检查定时器失败: %s", esp_err_to_name(error));
    }
}

#ifdef CONFIG_DESKMATE_TIME_SNTP_ENABLED
/** @brief 在网络 Application Task 上下文执行一次有界 SNTP 校时 */
static void sync_system_clock(void)
{
    network_manager_status_t status = { 0 };
    if (network_manager_get_status_copy(&status) != ESP_OK || status.state != NETWORK_STATE_ONLINE)
    {
        return;
    }
    const esp_err_t error =
        system_clock_sync_from_sntp(CONFIG_DESKMATE_TIME_SNTP_PRIMARY_SERVER, NETWORK_TIME_SYNC_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "SNTP 校时失败: %s", esp_err_to_name(error));
    }
}
#endif

/** @brief 启动一个新的 Network Manager 会话 */
static esp_err_t start_network_session(void)
{
    ESP_RETURN_ON_ERROR(network_manager_set_notify_callback_borrow(on_network_manager_notify, NULL),
                        TAG,
                        "注册网络状态通知失败");
    return network_manager_start();
}

/** @brief 启动初始 Network Manager 会话并在同步失败时进入顶层退避 */
static void handle_start_session_command(void)
{
    const esp_err_t error = start_network_session();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "启动网络会话失败: %s", esp_err_to_name(error));
        schedule_reconnect_backoff();
    }
}

/** @brief 停止已结束策略的会话并建立下一轮 Network Manager 会话 */
static void handle_restart_session_command(void)
{
    if (light_sleep_is_suspended())
    {
        return;
    }

    network_manager_status_t status = { 0 };
    esp_err_t                error  = network_manager_get_status_copy(&status);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "重启前读取网络状态失败: %s", esp_err_to_name(error));
        schedule_reconnect_backoff();
        return;
    }
    if (status.state != NETWORK_STATE_ERROR && status.state != NETWORK_STATE_STOPPED)
    {
        reset_reconnect_policy();
        return;
    }
    error = stop_remote_log_upload();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "停止旧远端日志上传失败: %s", esp_err_to_name(error));
        schedule_reconnect_backoff();
        return;
    }
    if (status.state != NETWORK_STATE_STOPPED)
    {
        error = network_manager_stop();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "停止旧网络会话失败: %s", esp_err_to_name(error));
            schedule_reconnect_backoff();
            return;
        }
    }

    error = start_network_session();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "重新启动网络会话失败: %s", esp_err_to_name(error));
        schedule_reconnect_backoff();
    }
}

/**
 * @brief Network Manager 快速通知回调，保留待收敛事实并尽力投递唤醒标记
 *
 * @param[in] ctx 未使用上下文
 */
static void on_network_manager_notify(void *ctx)
{
    (void) ctx;
    taskENTER_CRITICAL(&s_state_lock);
    const bool should_post   = !s_manager_change_pending;
    s_manager_change_pending = true;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!should_post)
    {
        return;
    }

    const network_command_t command = {
        .type = NETWORK_COMMAND_MANAGER_CHANGED,
    };
    if (post_control_command(&command) != ESP_OK)
    {
        ESP_LOGW(TAG, "网络状态通知命令队列已满，保留待收敛标志");
    }
}

/**
 * @brief 在状态锁内按 Network Manager 事实收敛 Portal 过渡占位
 *
 * PROVISIONING/VALIDATING 表示异步请求已经生效；ERROR/STOPPED/STOPPING 表示请求或会话
 * 已经失败终止。若 Portal 状态通知被合并并直接观察到 ONLINE，则活动序号前进可以证明
 * Portal 提交和验证已经发生。其他状态不能证明已消费 Portal 请求，必须继续保留占位。
 *
 * @param[in] status 最新 Network Manager 状态值快照
 */
static void reconcile_portal_transition_locked(const network_manager_status_t *status)
{
    if (!s_portal_transition_pending)
    {
        return;
    }

    const bool portal_started =
        status->state == NETWORK_STATE_PROVISIONING || status->state == NETWORK_STATE_VALIDATING;
    const bool transition_failed = status->state == NETWORK_STATE_ERROR || status->state == NETWORK_STATE_STOPPED
                                   || status->state == NETWORK_STATE_STOPPING;
    const bool portal_completed  = status->state == NETWORK_STATE_ONLINE
                                   && status->portal_activity_sequence != s_portal_transition_activity_sequence;
    if (portal_started || transition_failed || portal_completed)
    {
        s_portal_transition_pending = false;
    }
}

/** @brief 把 Network Manager 状态事实转换为 DeskMate 顶层连接策略 */
static void handle_manager_changed_command(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_manager_change_pending = false;
    taskEXIT_CRITICAL(&s_state_lock);

    network_manager_status_t status = { 0 };
    const esp_err_t          error  = network_manager_get_status_copy(&status);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "读取网络状态变化失败: %s", esp_err_to_name(error));
        return;
    }

    const network_manager_state_t previous = s_last_manager_state;
    s_last_manager_state                   = status.state;
    app_network_link_change_callback_t link_change_callback;
    void                              *link_change_context;
    taskENTER_CRITICAL(&s_state_lock);
    reconcile_portal_transition_locked(&status);
    link_change_callback = s_link_change_callback;
    link_change_context  = s_link_change_callback_context;
    taskEXIT_CRITICAL(&s_state_lock);
    if (link_change_callback != NULL)
    {
        link_change_callback(link_change_context);
    }
    (void) presentation_dispatch_status_update();

    if (status.state == NETWORK_STATE_ONLINE)
    {
        reset_reconnect_policy();
        if (previous != NETWORK_STATE_ONLINE)
        {
            start_remote_log_upload();
#ifdef CONFIG_DESKMATE_TIME_SNTP_ENABLED
            const network_command_t time_command = {
                .type = NETWORK_COMMAND_SYNC_TIME,
            };
            if (post_control_command(&time_command) != ESP_OK)
            {
                ESP_LOGW(TAG, "投递首次 SNTP 校时命令失败");
            }
            if (s_time_sync_timer != NULL && CONFIG_DESKMATE_TIME_SNTP_SYNC_INTERVAL_SEC > 0)
            {
                (void) esp_timer_stop(s_time_sync_timer);
                const esp_err_t timer_error =
                    esp_timer_start_periodic(s_time_sync_timer,
                                             (uint64_t) CONFIG_DESKMATE_TIME_SNTP_SYNC_INTERVAL_SEC * 1000000ULL);
                if (timer_error != ESP_OK)
                {
                    ESP_LOGW(TAG, "启动 SNTP 周期定时器失败: %s", esp_err_to_name(timer_error));
                }
            }
#endif
            taskENTER_CRITICAL(&s_state_lock);
            const bool periodic_enabled = s_periodic_enabled;
            taskEXIT_CRITICAL(&s_state_lock);
            const esp_err_t sync_error = periodic_enabled ? app_network_request_sync() : ESP_ERR_INVALID_STATE;
            if (sync_error != ESP_OK && sync_error != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGW(TAG, "联网后投递 Dashboard 同步失败: %s", esp_err_to_name(sync_error));
            }
        }
        return;
    }

#ifdef CONFIG_DESKMATE_TIME_SNTP_ENABLED
    if (s_time_sync_timer != NULL)
    {
        (void) esp_timer_stop(s_time_sync_timer);
    }
#endif
    set_dashboard_online(false);

    if (status.state == NETWORK_STATE_PROVISIONING || status.state == NETWORK_STATE_VALIDATING)
    {
        reset_reconnect_policy();
        return;
    }
    if (status.state == NETWORK_STATE_ERROR)
    {
        bool            has_saved_config = false;
        const esp_err_t saved_error      = network_manager_has_saved_config(&has_saved_config);
        if (status.last_error == ESP_ERR_NOT_FOUND || (saved_error == ESP_OK && !has_saved_config))
        {
            reset_reconnect_policy();
            taskENTER_CRITICAL(&s_state_lock);
            const bool conflict = s_active_lease_type != APP_NETWORK_LEASE_NONE || s_portal_transition_pending;
            if (!conflict)
            {
                s_portal_transition_pending           = true;
                s_portal_transition_activity_sequence = status.portal_activity_sequence;
            }
            taskEXIT_CRITICAL(&s_state_lock);
            if (conflict)
            {
                ESP_LOGW(TAG, "互斥网络产品租约或 Portal 过渡期间拒绝自动进入配网");
                return;
            }
            const esp_err_t portal_error = network_manager_request_start_portal();
            if (portal_error != ESP_OK)
            {
                taskENTER_CRITICAL(&s_state_lock);
                s_portal_transition_pending = false;
                taskEXIT_CRITICAL(&s_state_lock);
                ESP_LOGE(TAG, "无可用网络配置且启动配网失败: %s", esp_err_to_name(portal_error));
                schedule_reconnect_backoff();
            }
            return;
        }
        schedule_reconnect_backoff();
        return;
    }
    if (status.state == NETWORK_STATE_STOPPED && !light_sleep_is_suspended() && previous != NETWORK_STATE_STOPPED)
    {
        schedule_reconnect_backoff();
    }
}

/** @brief 在 Network Manager 中显式切换到配网 Portal */
static void handle_start_portal_command(void)
{
    network_manager_status_t status       = { 0 };
    const esp_err_t          status_error = network_manager_get_status_copy(&status);
    if (status_error != ESP_OK)
    {
        ESP_LOGW(TAG, "切换配网前读取网络状态失败: %s", esp_err_to_name(status_error));
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    reconcile_portal_transition_locked(&status);
    const bool conflict = s_active_lease_type != APP_NETWORK_LEASE_NONE || s_portal_transition_pending || s_ota_queued
                          || s_ota_running || s_light_sleep_suspended;
    if (!conflict)
    {
        s_portal_transition_pending           = true;
        s_portal_transition_activity_sequence = status.portal_activity_sequence;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (conflict)
    {
        ESP_LOGW(TAG, "互斥网络产品租约、Portal 过渡、OTA 或轻睡眠事务期间拒绝切换配网");
        return;
    }

    reset_reconnect_policy();
    const esp_err_t error = network_manager_request_start_portal();
    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_portal_transition_pending = false;
        taskEXIT_CRITICAL(&s_state_lock);
        ESP_LOGW(TAG, "启动配网 Portal 失败: %s", esp_err_to_name(error));
    }
}

/** @brief 在 Dashboard 重试等待中响应取消请求 */
static bool wait_sync_retry_delay(uint32_t delay_ms)
{
    uint32_t remaining_ms = delay_ms;
    while (remaining_ms > 0U)
    {
        if (is_sync_cancel_requested())
        {
            return false;
        }
        const uint32_t slice_ms =
            remaining_ms > NETWORK_MANAGER_POLL_INTERVAL_MS ? NETWORK_MANAGER_POLL_INTERVAL_MS : remaining_ms;
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
        remaining_ms -= slice_ms;
    }
    return true;
}

/**
 * @brief 执行带联网等待、重试和取消的 DeskMate Dashboard 同步
 *
 * @return ESP_OK Dashboard 与下一联网截止时间均已更新；其他值表示取消、冲突或同步失败
 */
static esp_err_t execute_sync_command(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_sync_queued = false;
    const bool should_run =
        !s_sync_cancel_requested && s_active_lease_type == APP_NETWORK_LEASE_NONE && !s_light_sleep_suspended;
    s_sync_running = should_run;
    taskEXIT_CRITICAL(&s_state_lock);

    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (should_run)
    {
        const int max_attempts = CONFIG_DESKMATE_API_SYNC_RETRY_MAX > 0 ? CONFIG_DESKMATE_API_SYNC_RETRY_MAX : 1;
        for (int attempt = 1; attempt <= max_attempts; ++attempt)
        {
            if (is_sync_cancel_requested())
            {
                error = ESP_ERR_INVALID_STATE;
                break;
            }

            error = ensure_network_online(NETWORK_CONNECT_TIMEOUT_MS, true);
            if (error == ESP_OK)
            {
                error = fetch_dashboard();
            }
            if (error == ESP_OK || attempt == max_attempts)
            {
                break;
            }

            const uint32_t delay_ms = (uint32_t) CONFIG_DESKMATE_API_SYNC_RETRY_DELAY_MS * (uint32_t) attempt;
            if (!wait_sync_retry_delay(delay_ms))
            {
                error = ESP_ERR_INVALID_STATE;
                break;
            }
        }

        if (error == ESP_OK)
        {
            refresh_dashboard_services();
            set_dashboard_online(true);
        }
        else if (!is_sync_cancel_requested())
        {
            set_dashboard_online(false);
            ESP_LOGW(TAG, "Dashboard 同步失败: %s", esp_err_to_name(error));
        }
        reschedule_periodic_timer();
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_sync_running          = false;
    s_sync_cancel_requested = false;
    taskEXIT_CRITICAL(&s_state_lock);
    return error;
}

/** @brief 执行无需回执的普通 Dashboard 同步命令 */
static void handle_sync_command(void)
{
    (void) execute_sync_command();
}

/** @brief 执行轻睡眠维护同步并在完整同步结束后返回回执 */
static void handle_maintenance_sync_command(const network_command_t *command)
{
    if (!begin_control_command(command))
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_sync_queued = false;
        taskEXIT_CRITICAL(&s_state_lock);
        complete_control_response(command, ESP_ERR_TIMEOUT);
        return;
    }

    const esp_err_t error = execute_sync_command();
    complete_control_response(command, error);
}

/** @brief 从 DeskMate 设置复制 OTA 服务身份到独立固件 OTA 工具 */
static esp_err_t configure_firmware_ota(void)
{
    device_settings_t settings;
    ESP_RETURN_ON_ERROR(settings_store_load_copy(&settings), TAG, "读取 OTA 服务配置失败");
    ESP_RETURN_ON_FALSE(settings.service_url[0] != '\0', ESP_ERR_INVALID_STATE, TAG, "OTA 服务地址为空");

    char device_id[DESKMATE_API_DEVICE_ID_MAX] = { 0 };
    ESP_RETURN_ON_ERROR(system_info_get_device_id(device_id, sizeof(device_id)), TAG, "生成 OTA 设备 ID 失败");
    return firmware_ota_configure_copy(settings.service_url, settings.device_token, device_id);
}

/** @brief 将 OTA 请求提交失败转换为统一完成事实 */
static void present_ota_request_failure(firmware_ota_event_type_t type, esp_err_t error, bool manual)
{
    if (type == FIRMWARE_OTA_EVENT_CHECK_COMPLETED)
    {
        ota_presenter_show_check_request_failed(error, manual);
        return;
    }
    ota_presenter_show_install_submit_failed(error);
}

/** @brief 向异步 Firmware OTA Task 提交检查或安装事务 */
static void handle_ota_command(bool install, app_network_ota_check_mode_t mode)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_ota_queued          = false;
    const bool should_run = s_active_lease_type == APP_NETWORK_LEASE_NONE && !s_light_sleep_suspended;
    s_ota_running         = should_run;
    s_ota_check_mode      = mode;
    taskEXIT_CRITICAL(&s_state_lock);

    if (!should_run)
    {
        ESP_LOGI(TAG, "互斥网络产品租约或轻睡眠事务期间跳过 OTA %s", install ? "安装" : "检查");
        present_ota_request_failure(install ? FIRMWARE_OTA_EVENT_INSTALL_COMPLETED : FIRMWARE_OTA_EVENT_CHECK_COMPLETED,
                                    ESP_ERR_INVALID_STATE,
                                    mode == APP_NETWORK_OTA_CHECK_MANUAL);
        reschedule_ota_timer();
        return;
    }

    esp_err_t error = ensure_network_online(NETWORK_CONNECT_TIMEOUT_MS, false);
    if (error == ESP_OK && !install)
    {
        error = configure_firmware_ota();
    }
    if (error == ESP_OK)
    {
        if (install)
        {
            ota_presenter_show_downloading();
            error = firmware_ota_request_install();
        }
        else
        {
            ota_presenter_show_checking();
            error = firmware_ota_request_check();
        }
    }
    if (error == ESP_OK)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_ota_running = false;
    taskEXIT_CRITICAL(&s_state_lock);
    present_ota_request_failure(install ? FIRMWARE_OTA_EVENT_INSTALL_COMPLETED : FIRMWARE_OTA_EVENT_CHECK_COMPLETED,
                                error,
                                mode == APP_NETWORK_OTA_CHECK_MANUAL);
    ESP_LOGW(TAG, "OTA %s请求失败: %s", install ? "安装" : "检查", esp_err_to_name(error));
    reschedule_ota_timer();
}

/**
 * @brief Firmware OTA 快速完成回调，只复制不可变事件并投递 Application 命令
 *
 * @param[in] event OTA 完成事件
 * @param[in] context 未使用上下文
 */
static void on_firmware_ota_event(const firmware_ota_event_t *event, void *context)
{
    (void) context;
    if (event == NULL)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    const app_network_ota_check_mode_t mode = s_ota_check_mode;
    taskEXIT_CRITICAL(&s_state_lock);
    const network_command_t command = {
        .type           = NETWORK_COMMAND_OTA_EVENT,
        .ota_check_mode = mode,
        .ota_event      = *event,
    };
    if (post_control_command(&command) == ESP_OK)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_ota_running        = false;
    s_ota_pending_update = event->type == FIRMWARE_OTA_EVENT_CHECK_COMPLETED && event->result == ESP_OK
                           && event->check_result.update_available;
    taskEXIT_CRITICAL(&s_state_lock);
    ota_presenter_handle_firmware_event_copy(event, mode == APP_NETWORK_OTA_CHECK_MANUAL);
    ESP_LOGE(TAG, "OTA 完成事实命令队列已满，已跳过自动安装策略");
}

/** @brief 处理异步 Firmware OTA 完成事实并决定是否自动安装 */
static void handle_ota_event_command(const network_command_t *command)
{
    const firmware_ota_event_t *event = &command->ota_event;
    const bool has_update             = event->type == FIRMWARE_OTA_EVENT_CHECK_COMPLETED && event->result == ESP_OK
                                        && event->check_result.update_available;

    taskENTER_CRITICAL(&s_state_lock);
    s_ota_running        = false;
    s_ota_pending_update = has_update;
    taskEXIT_CRITICAL(&s_state_lock);
    ota_presenter_handle_firmware_event_copy(event, command->ota_check_mode == APP_NETWORK_OTA_CHECK_MANUAL);

    if (!has_update)
    {
        reschedule_ota_timer();
        return;
    }

    ESP_LOGI(TAG,
             "发现 DeskMate 固件更新: version=%s ota_version=%llu",
             event->check_result.target_version,
             (unsigned long long) event->check_result.target_ota_version);
    if (command->ota_check_mode != APP_NETWORK_OTA_CHECK_AUTOMATIC)
    {
        return;
    }

    device_settings_t settings;
    const esp_err_t   settings_error = settings_store_load_copy(&settings);
    if (settings_error != ESP_OK)
    {
        ESP_LOGW(TAG, "读取 OTA 自动安装设置失败: %s", esp_err_to_name(settings_error));
        return;
    }
    if (!settings.ota_auto_install)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    const bool can_install = s_active_lease_type == APP_NETWORK_LEASE_NONE && !s_light_sleep_suspended;
    if (can_install)
    {
        s_ota_running    = true;
        s_ota_check_mode = command->ota_check_mode;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!can_install)
    {
        ESP_LOGI(TAG, "互斥网络产品租约或轻睡眠事务阻止 OTA 自动安装，保留待安装目标");
        return;
    }

    ota_presenter_show_downloading();
    const esp_err_t error = firmware_ota_request_install();
    if (error == ESP_OK)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_ota_running = false;
    taskEXIT_CRITICAL(&s_state_lock);
    present_ota_request_failure(FIRMWARE_OTA_EVENT_INSTALL_COMPLETED, error, false);
    ESP_LOGW(TAG, "提交 OTA 自动安装失败: %s", esp_err_to_name(error));
}

/** @brief 判断租约类型是否对应一个可申请的互斥网络产品 */
static bool network_lease_type_is_supported(app_network_lease_type_t type)
{
    return type == APP_NETWORK_LEASE_REALTIME_VOICE || type == APP_NETWORK_LEASE_WEB_FILE;
}

/** @brief 在网络 Application Task 上下文仲裁并授予一个带类型的互斥网络产品租约 */
static void handle_lease_acquire_command(const network_command_t *command)
{
    network_manager_status_t status      = { 0 };
    const esp_err_t          status_err  = network_manager_get_status_copy(&status);
    const int64_t            now_us      = esp_timer_get_time();
    SemaphoreHandle_t        signal      = NULL;
    bool                     granted     = false;
    uint32_t                 granted_gen = 0U;

    taskENTER_CRITICAL(&s_state_lock);
    if (command->response_slot < NETWORK_CONTROL_RESPONSE_SLOTS)
    {
        control_response_slot_t *slot = &s_control_slots[command->response_slot];
        if (slot->request_id == command->request_id && slot->state == CONTROL_RESPONSE_PENDING)
        {
            esp_err_t result = ESP_ERR_INVALID_STATE;
            if (now_us >= command->deadline_us)
            {
                result = ESP_ERR_TIMEOUT;
            }
            else if (!network_lease_type_is_supported(command->lease_type))
            {
                result = ESP_ERR_INVALID_ARG;
            }
            else if (status_err != ESP_OK || status.state != NETWORK_STATE_ONLINE)
            {
                result = ESP_ERR_INVALID_STATE;
            }
            else
            {
                reconcile_portal_transition_locked(&status);
                if (s_light_sleep_suspended || s_active_lease_type != APP_NETWORK_LEASE_NONE
                    || s_portal_transition_pending || s_ota_queued || s_ota_running || s_next_lease_generation == 0U)
                {
                    result = ESP_ERR_INVALID_STATE;
                }
                else
                {
                    granted_gen = s_next_lease_generation;
                    if (s_next_lease_generation == UINT32_MAX)
                    {
                        s_next_lease_generation = 0U;
                    }
                    else
                    {
                        ++s_next_lease_generation;
                    }
                    s_active_lease_generation = granted_gen;
                    s_active_lease_type       = command->lease_type;
                    s_sync_cancel_requested   = s_sync_queued || s_sync_running;
                    result                    = ESP_OK;
                    granted                   = true;
                }
            }
            signal = complete_control_response_locked(command->response_slot, command->request_id, result, granted_gen);
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (granted)
    {
        reschedule_periodic_timer();
        reschedule_ota_timer();
        ESP_LOGI(TAG,
                 "已授予互斥网络产品租约: 类型=%u，代次=%lu",
                 (unsigned) command->lease_type,
                 (unsigned long) granted_gen);
    }
    if (signal != NULL)
    {
        (void) xSemaphoreGive(signal);
    }
}

/** @brief 在网络 Application Task 上下文按类型和代次释放互斥网络产品租约 */
static void handle_lease_release_command(const network_command_t *command)
{
    SemaphoreHandle_t signal   = NULL;
    bool              released = false;
    const int64_t     now_us   = esp_timer_get_time();

    taskENTER_CRITICAL(&s_state_lock);
    if (command->response_slot < NETWORK_CONTROL_RESPONSE_SLOTS)
    {
        control_response_slot_t *slot = &s_control_slots[command->response_slot];
        if (slot->request_id == command->request_id && slot->state == CONTROL_RESPONSE_PENDING
            && now_us < command->deadline_us)
        {
            esp_err_t result = ESP_OK;
            slot->state      = CONTROL_RESPONSE_EXECUTING;
            if (s_active_lease_type != APP_NETWORK_LEASE_NONE)
            {
                if (command->lease_type != s_active_lease_type
                    || command->lease_generation != s_active_lease_generation)
                {
                    result = ESP_ERR_INVALID_STATE;
                }
                else
                {
                    s_active_lease_type       = APP_NETWORK_LEASE_NONE;
                    s_active_lease_generation = 0U;
                    released                  = true;
                }
            }
            signal = complete_control_response_locked(command->response_slot, command->request_id, result, 0U);
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (released)
    {
        ESP_LOGI(TAG,
                 "已释放互斥网络产品租约: 类型=%u，代次=%lu",
                 (unsigned) command->lease_type,
                 (unsigned long) command->lease_generation);
        reschedule_periodic_timer();
        reschedule_ota_timer();
    }
    if (signal != NULL)
    {
        (void) xSemaphoreGive(signal);
    }
}

/** @brief 停止所有可能在轻睡眠后补发命令的网络产品 Timer */
static void stop_network_policy_timers(void)
{
    esp_timer_handle_t timers[] = {
        s_periodic_timer,
        s_reconnect_timer,
        s_ota_timer,
        s_time_sync_timer,
    };
    for (size_t index = 0U; index < sizeof(timers) / sizeof(timers[0]); ++index)
    {
        if (timers[index] != NULL)
        {
            (void) esp_timer_stop(timers[index]);
        }
    }
    s_reconnect_backoff_ms = 0U;
}

/** @brief 同步停止 Network Manager 会话并进入轻睡眠暂停态 */
static void handle_suspend_for_light_sleep_command(const network_command_t *command)
{
    if (!begin_control_command(command))
    {
        complete_control_response(command, ESP_ERR_TIMEOUT);
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    const bool already_suspended = s_light_sleep_suspended;
    const bool conflict          = s_active_lease_type != APP_NETWORK_LEASE_NONE || s_ota_queued || s_ota_running;
    if (!already_suspended && !conflict)
    {
        s_light_sleep_suspended = true;
        s_sync_cancel_requested = s_sync_queued || s_sync_running;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (already_suspended)
    {
        complete_control_response(command, ESP_OK);
        return;
    }
    if (conflict)
    {
        complete_control_response(command, ESP_ERR_INVALID_STATE);
        return;
    }

    stop_network_policy_timers();
    esp_err_t error = stop_remote_log_upload();
    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_light_sleep_suspended = false;
        s_sync_cancel_requested = false;
        taskEXIT_CRITICAL(&s_state_lock);
        reschedule_periodic_timer();
        reschedule_ota_timer();
        ESP_LOGE(TAG, "停止远端日志以进入轻睡眠失败: %s", esp_err_to_name(error));
        complete_control_response(command, error);
        return;
    }
    error = network_manager_stop();
    if (error != ESP_OK)
    {
        network_manager_status_t status = { 0 };
        if (network_manager_get_status_copy(&status) == ESP_OK && status.state == NETWORK_STATE_STOPPED)
        {
            error = ESP_OK;
        }
    }
    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_light_sleep_suspended = false;
        s_sync_cancel_requested = false;
        taskEXIT_CRITICAL(&s_state_lock);
        reschedule_periodic_timer();
        reschedule_ota_timer();
        ESP_LOGE(TAG, "暂停网络以进入轻睡眠失败: %s", esp_err_to_name(error));
        complete_control_response(command, error);
        return;
    }

    set_dashboard_online(false);
    (void) presentation_dispatch_status_update();
    ESP_LOGI(TAG, "网络会话已停止，允许进入轻睡眠");
    complete_control_response(command, ESP_OK);
}

/** @brief 重新启动 Network Manager 会话并恢复轻睡眠前产品策略 */
static void handle_resume_from_light_sleep_command(const network_command_t *command)
{
    if (!begin_control_command(command))
    {
        complete_control_response(command, ESP_ERR_TIMEOUT);
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    const bool was_suspended = s_light_sleep_suspended;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!was_suspended)
    {
        complete_control_response(command, ESP_OK);
        return;
    }

    const esp_err_t error = start_network_session();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "轻睡眠唤醒后启动网络会话失败: %s", esp_err_to_name(error));
        complete_control_response(command, error);
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_light_sleep_suspended = false;
    s_sync_cancel_requested = false;
    taskEXIT_CRITICAL(&s_state_lock);
    reschedule_periodic_timer();
    reschedule_ota_timer();
    ESP_LOGI(TAG, "轻睡眠唤醒后网络产品策略已恢复");
    complete_control_response(command, ESP_OK);
}

/** @brief 丢弃暂停期间遗留的普通网络命令并释放其排队状态 */
static void discard_command_while_suspended(const network_command_t *command)
{
    SemaphoreHandle_t signal = NULL;
    taskENTER_CRITICAL(&s_state_lock);
    switch (command->type)
    {
        case NETWORK_COMMAND_SYNC:
            s_sync_queued           = false;
            s_sync_cancel_requested = false;
            break;
        case NETWORK_COMMAND_MAINTENANCE_SYNC:
            s_sync_queued           = false;
            s_sync_cancel_requested = false;
            signal                  = complete_control_response_locked(command->response_slot,
                                                                       command->request_id,
                                                                       ESP_ERR_INVALID_STATE,
                                                                       0U);
            break;
        case NETWORK_COMMAND_OTA_CHECK:
        case NETWORK_COMMAND_OTA_INSTALL:
            s_ota_queued = false;
            break;
        case NETWORK_COMMAND_LEASE_ACQUIRE:
        case NETWORK_COMMAND_LEASE_RELEASE:
            signal = complete_control_response_locked(command->response_slot,
                                                      command->request_id,
                                                      ESP_ERR_INVALID_STATE,
                                                      0U);
            break;
        default:
            break;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (signal != NULL)
    {
        (void) xSemaphoreGive(signal);
    }
}

/**
 * @brief 唯一网络 Application Task，优先收敛耐久 Manager 事实并串行解释产品命令
 *
 * 每次可能阻塞于命令队列前先检查 Manager pending 标志。通知 marker 只负责唤醒，队列满
 * 不会丢失该标志；已由主动检查消费的旧 marker 会被幂等跳过。
 */
static void app_network_task(void *arg)
{
    (void) arg;
    task_stack_stats_t stack_stats = TASK_STACK_STATS_INITIALIZER;
    network_command_t  command;
    for (;;)
    {
        task_stack_stats_log_if_due(&stack_stats, "app_network_task");

        taskENTER_CRITICAL(&s_state_lock);
        const bool manager_change_pending = s_manager_change_pending;
        taskEXIT_CRITICAL(&s_state_lock);
        if (manager_change_pending)
        {
            handle_manager_changed_command();
            continue;
        }

        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (command.type == NETWORK_COMMAND_MANAGER_CHANGED)
        {
            taskENTER_CRITICAL(&s_state_lock);
            const bool marker_is_current = s_manager_change_pending;
            taskEXIT_CRITICAL(&s_state_lock);
            if (marker_is_current)
            {
                handle_manager_changed_command();
            }
            continue;
        }
        if (command.type == NETWORK_COMMAND_SUSPEND_FOR_LIGHT_SLEEP)
        {
            handle_suspend_for_light_sleep_command(&command);
            continue;
        }
        if (command.type == NETWORK_COMMAND_RESUME_FROM_LIGHT_SLEEP)
        {
            handle_resume_from_light_sleep_command(&command);
            continue;
        }
        if (light_sleep_is_suspended())
        {
            discard_command_while_suspended(&command);
            continue;
        }

        switch (command.type)
        {
            case NETWORK_COMMAND_START_SESSION:
                handle_start_session_command();
                break;
            case NETWORK_COMMAND_RESTART_SESSION:
                handle_restart_session_command();
                break;
            case NETWORK_COMMAND_START_PORTAL:
                handle_start_portal_command();
                break;
            case NETWORK_COMMAND_SYNC:
                handle_sync_command();
                break;
            case NETWORK_COMMAND_MAINTENANCE_SYNC:
                handle_maintenance_sync_command(&command);
                break;
            case NETWORK_COMMAND_OTA_CHECK:
                handle_ota_command(false, command.ota_check_mode);
                break;
            case NETWORK_COMMAND_OTA_INSTALL:
                handle_ota_command(true, command.ota_check_mode);
                break;
            case NETWORK_COMMAND_OTA_EVENT:
                handle_ota_event_command(&command);
                break;
            case NETWORK_COMMAND_SYNC_TIME:
#ifdef CONFIG_DESKMATE_TIME_SNTP_ENABLED
                sync_system_clock();
#endif
                break;
            case NETWORK_COMMAND_LEASE_ACQUIRE:
                handle_lease_acquire_command(&command);
                break;
            case NETWORK_COMMAND_LEASE_RELEASE:
                handle_lease_release_command(&command);
                break;
            default:
                ESP_LOGW(TAG, "忽略未知网络产品命令: %d", (int) command.type);
                break;
        }
    }
}

/** @brief Dashboard 周期 Timer 回调，只投递同步命令 */
static void periodic_timer_cb(void *arg)
{
    (void) arg;
    (void) app_network_request_sync();
}

/** @brief 网络会话退避 Timer 回调，只投递重启命令 */
static void reconnect_timer_cb(void *arg)
{
    (void) arg;
    const network_command_t command = {
        .type = NETWORK_COMMAND_RESTART_SESSION,
    };
    (void) post_control_command(&command);
}

/** @brief OTA 自动检查 Timer 回调，只投递静默检查命令 */
static void ota_timer_cb(void *arg)
{
    (void) arg;
    (void) app_network_request_ota_check(APP_NETWORK_OTA_CHECK_AUTOMATIC);
}

/** @brief SNTP 周期 Timer 回调，只投递校时命令 */
static void time_sync_timer_cb(void *arg)
{
    (void) arg;
    const network_command_t command = {
        .type = NETWORK_COMMAND_SYNC_TIME,
    };
    (void) post_control_command(&command);
}

/** @brief 删除初始化期间已经创建的网络产品 Timer */
static void delete_created_timers(void)
{
    esp_timer_handle_t *timers[] = {
        &s_periodic_timer,
        &s_reconnect_timer,
        &s_ota_timer,
        &s_time_sync_timer,
    };
    for (size_t index = 0U; index < sizeof(timers) / sizeof(timers[0]); ++index)
    {
        if (*timers[index] != NULL)
        {
            (void) esp_timer_delete(*timers[index]);
            *timers[index] = NULL;
        }
    }
}

/** @brief 创建网络 Application 使用的全部 Timer */
static esp_err_t create_network_timers(void)
{
    const esp_timer_create_args_t periodic_args = {
        .callback = periodic_timer_cb,
        .name     = "app_dashboard",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&periodic_args, &s_periodic_timer), TAG, "创建 Dashboard Timer 失败");

    const esp_timer_create_args_t reconnect_args = {
        .callback = reconnect_timer_cb,
        .name     = "app_network_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_args, &s_reconnect_timer), TAG, "创建网络退避 Timer 失败");

    const esp_timer_create_args_t ota_args = {
        .callback = ota_timer_cb,
        .name     = "app_ota",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ota_args, &s_ota_timer), TAG, "创建 OTA Timer 失败");

    const esp_timer_create_args_t time_args = {
        .callback = time_sync_timer_cb,
        .name     = "app_time_sync",
    };
    return esp_timer_create(&time_args, &s_time_sync_timer);
}

esp_err_t app_network_init(void)
{
    if (s_command_queue != NULL && s_task != NULL)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(initialize_control_response_slots(), TAG, "初始化网络控制回执失败");
    s_command_queue = xQueueCreate(NETWORK_COMMAND_QUEUE_LENGTH, sizeof(network_command_t));
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_NO_MEM, TAG, "创建网络命令队列失败");

    const network_manager_config_store_t config_store = {
        .load_config_copy   = load_network_config,
        .save_config_borrow = save_network_config,
        .erase_config       = erase_network_config,
        .ctx                = NULL,
    };
    esp_err_t error = network_manager_init_borrow(&config_store);
    if (error != ESP_OK)
    {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return error;
    }

    const firmware_ota_config_t ota_config = {
        .check_timeout_ms    = CONFIG_DESKMATE_OTA_HTTP_TIMEOUT_MS,
        .download_timeout_ms = CONFIG_DESKMATE_OTA_HTTP_TIMEOUT_MS,
    };
    error = firmware_ota_init(&ota_config);
    if (error != ESP_OK)
    {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return error;
    }
    error = firmware_ota_set_event_callback_borrow(on_firmware_ota_event, NULL);
    if (error == ESP_OK)
    {
        error = firmware_ota_start();
    }
    if (error == ESP_OK)
    {
        error = create_network_timers();
    }
    if (error != ESP_OK)
    {
        delete_created_timers();
        firmware_ota_state_t ota_state = FIRMWARE_OTA_STATE_STOPPED;
        if (firmware_ota_get_state_copy(&ota_state) == ESP_OK && ota_state != FIRMWARE_OTA_STATE_STOPPED)
        {
            (void) firmware_ota_stop();
        }
        (void) firmware_ota_set_event_callback_borrow(NULL, NULL);
        (void) firmware_ota_deinit();
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return error;
    }

    if (xTaskCreate(app_network_task, "app_network_task", NETWORK_TASK_STACK, NULL, NETWORK_TASK_PRIORITY, &s_task)
        != pdPASS)
    {
        delete_created_timers();
        (void) firmware_ota_stop();
        (void) firmware_ota_set_event_callback_borrow(NULL, NULL);
        (void) firmware_ota_deinit();
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    const network_command_t start_command = {
        .type = NETWORK_COMMAND_START_SESSION,
    };
    if (post_control_command(&start_command) != ESP_OK)
    {
        ESP_LOGW(TAG, "初始网络会话命令投递失败");
    }
    ESP_LOGI(TAG, "网络 Application 初始化完成");
    return ESP_OK;
}

esp_err_t app_network_set_link_change_callback_borrow(app_network_link_change_callback_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG, "网络链路变化回调为空");

    taskENTER_CRITICAL(&s_state_lock);
    if (s_task == NULL)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_link_change_callback != NULL
        && (s_link_change_callback != callback || s_link_change_callback_context != context))
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_link_change_callback         = callback;
    s_link_change_callback_context = context;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t app_network_request_sync(void)
{
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "网络任务未初始化");

    taskENTER_CRITICAL(&s_state_lock);
    if (s_light_sleep_suspended || s_active_lease_type != APP_NETWORK_LEASE_NONE || s_sync_queued || s_sync_running)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_sync_queued           = true;
    s_sync_cancel_requested = false;
    taskEXIT_CRITICAL(&s_state_lock);

    const network_command_t command = {
        .type = NETWORK_COMMAND_SYNC,
    };
    const esp_err_t error = post_control_command(&command);
    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_sync_queued = false;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    return error;
}

esp_err_t app_network_cancel_sync(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_sync_cancel_requested = s_sync_queued || s_sync_running;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

/** @brief 投递一个需要同步回执的通用网络控制命令 */
static esp_err_t request_control_response_command(network_command_type_t type, app_network_lease_type_t lease_type,
                                                  uint32_t generation, uint32_t timeout_ms, uint32_t *out_generation)
{
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "网络任务未初始化");
    ESP_RETURN_ON_FALSE(timeout_ms > 0U, ESP_ERR_INVALID_ARG, TAG, "网络控制等待时间无效");

    uint8_t  slot_index;
    uint32_t request_id;
    ESP_RETURN_ON_ERROR(allocate_control_response_slot(&slot_index, &request_id), TAG, "没有可用的网络控制回执槽");

    const int64_t           deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    const network_command_t command     = {
        .type             = type,
        .lease_type       = lease_type,
        .response_slot    = slot_index,
        .request_id       = request_id,
        .lease_generation = generation,
        .deadline_us      = deadline_us,
    };
    const esp_err_t error = post_control_command(&command);
    if (error != ESP_OK)
    {
        release_unposted_control_response_slot(slot_index, request_id);
        return error;
    }
    return wait_control_response(slot_index, request_id, deadline_us, out_generation);
}

esp_err_t app_network_sync_for_light_sleep(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "网络任务未初始化");
    ESP_RETURN_ON_FALSE(timeout_ms > 0U, ESP_ERR_INVALID_ARG, TAG, "维护同步等待时间无效");

    uint8_t  slot_index;
    uint32_t request_id;
    ESP_RETURN_ON_ERROR(allocate_control_response_slot(&slot_index, &request_id), TAG, "没有可用的维护同步回执槽");

    taskENTER_CRITICAL(&s_state_lock);
    const bool allowed = !s_light_sleep_suspended && s_active_lease_type == APP_NETWORK_LEASE_NONE && !s_sync_queued
                         && !s_sync_running && !s_ota_queued && !s_ota_running;
    if (allowed)
    {
        s_sync_queued           = true;
        s_sync_cancel_requested = false;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    if (!allowed)
    {
        release_unposted_control_response_slot(slot_index, request_id);
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t           deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    const network_command_t command     = {
        .type          = NETWORK_COMMAND_MAINTENANCE_SYNC,
        .response_slot = slot_index,
        .request_id    = request_id,
        .deadline_us   = deadline_us,
    };
    const esp_err_t error = post_control_command(&command);
    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_sync_queued = false;
        taskEXIT_CRITICAL(&s_state_lock);
        release_unposted_control_response_slot(slot_index, request_id);
        return error;
    }
    return wait_control_response(slot_index, request_id, deadline_us, NULL);
}

esp_err_t app_network_get_next_refresh_at_utc(int64_t *out_utc_timestamp)
{
    ESP_RETURN_ON_FALSE(out_utc_timestamp != NULL, ESP_ERR_INVALID_ARG, TAG, "下一刷新时间输出为空");

    taskENTER_CRITICAL(&s_state_lock);
    const int64_t next_refresh_at_utc = s_next_refresh_at_utc;
    taskEXIT_CRITICAL(&s_state_lock);
    if (next_refresh_at_utc <= 0)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *out_utc_timestamp = next_refresh_at_utc;
    return ESP_OK;
}

esp_err_t app_network_suspend_for_light_sleep(uint32_t timeout_ms)
{
    (void) app_network_cancel_sync();
    return request_control_response_command(NETWORK_COMMAND_SUSPEND_FOR_LIGHT_SLEEP,
                                            APP_NETWORK_LEASE_NONE,
                                            0U,
                                            timeout_ms,
                                            NULL);
}

esp_err_t app_network_resume_from_light_sleep(uint32_t timeout_ms)
{
    return request_control_response_command(NETWORK_COMMAND_RESUME_FROM_LIGHT_SLEEP,
                                            APP_NETWORK_LEASE_NONE,
                                            0U,
                                            timeout_ms,
                                            NULL);
}

void app_network_set_periodic_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_periodic_enabled = enabled;
    taskEXIT_CRITICAL(&s_state_lock);
    reschedule_periodic_timer();
}

void app_network_set_ota_auto_check_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_ota_auto_check_enabled = enabled;
    taskEXIT_CRITICAL(&s_state_lock);
    reschedule_ota_timer();
}

/** @brief 投递 OTA 命令并阻止工具状态不兼容或重复排队 */
static esp_err_t request_ota_command(network_command_type_t type, app_network_ota_check_mode_t mode)
{
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "网络任务未初始化");
    ESP_RETURN_ON_FALSE(type != NETWORK_COMMAND_OTA_CHECK || (unsigned) mode <= APP_NETWORK_OTA_CHECK_AUTOMATIC,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "OTA 检查来源无效");

    firmware_ota_state_t ota_state;
    ESP_RETURN_ON_ERROR(firmware_ota_get_state_copy(&ota_state), TAG, "读取 OTA 状态失败");
    const bool state_allowed =
        (type == NETWORK_COMMAND_OTA_CHECK && ota_state == FIRMWARE_OTA_STATE_IDLE)
        || (type == NETWORK_COMMAND_OTA_INSTALL && ota_state == FIRMWARE_OTA_STATE_UPDATE_AVAILABLE);
    ESP_RETURN_ON_FALSE(state_allowed, ESP_ERR_INVALID_STATE, TAG, "OTA 当前状态不接受该请求");

    taskENTER_CRITICAL(&s_state_lock);
    if (s_light_sleep_suspended || s_active_lease_type != APP_NETWORK_LEASE_NONE || s_ota_queued || s_ota_running)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ota_queued = true;
    taskEXIT_CRITICAL(&s_state_lock);

    const network_command_t command = {
        .type           = type,
        .ota_check_mode = mode,
    };
    const esp_err_t error = post_control_command(&command);
    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_ota_queued = false;
        taskEXIT_CRITICAL(&s_state_lock);
        reschedule_ota_timer();
        return error;
    }
    reschedule_ota_timer();
    return ESP_OK;
}

esp_err_t app_network_request_ota_check(app_network_ota_check_mode_t mode)
{
    return request_ota_command(NETWORK_COMMAND_OTA_CHECK, mode);
}

esp_err_t app_network_request_ota_install(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const app_network_ota_check_mode_t mode = s_ota_check_mode;
    taskEXIT_CRITICAL(&s_state_lock);
    return request_ota_command(NETWORK_COMMAND_OTA_INSTALL, mode);
}

esp_err_t app_network_discard_ota_update(void)
{
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "网络任务未初始化");

    taskENTER_CRITICAL(&s_state_lock);
    const bool transaction_active = s_ota_queued || s_ota_running;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(!transaction_active, ESP_ERR_INVALID_STATE, TAG, "OTA 事务执行期间不能丢弃目标");

    ESP_RETURN_ON_ERROR(firmware_ota_discard_pending_update(), TAG, "丢弃 Firmware OTA 目标失败");
    taskENTER_CRITICAL(&s_state_lock);
    s_ota_pending_update = false;
    taskEXIT_CRITICAL(&s_state_lock);
    reschedule_ota_timer();
    return ESP_OK;
}

bool app_network_is_ota_busy(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    bool busy = s_ota_queued || s_ota_running;
    taskEXIT_CRITICAL(&s_state_lock);
    if (busy)
    {
        return true;
    }

    firmware_ota_state_t state;
    if (firmware_ota_get_state_copy(&state) == ESP_OK)
    {
        busy = state == FIRMWARE_OTA_STATE_CHECKING || state == FIRMWARE_OTA_STATE_DOWNLOADING
               || state == FIRMWARE_OTA_STATE_AWAITING_RESTART;
    }
    return busy;
}

/**
 * @brief 同步申请一个带类型的互斥网络产品租约
 *
 * 调用方只持有返回的代次值；租约状态和回执槽始终由网络 Application Task 拥有。
 *
 * @param[in] type 待申请的具体产品租约类型
 * @param[in] timeout_ms 最长等待回执时间
 * @param[out] out_generation 成功时输出租约代次
 * @return ESP_OK 已授予；或参数、网络状态、产品冲突、代次耗尽、回执资源及超时错误码
 */
static esp_err_t acquire_network_lease(app_network_lease_type_t type, uint32_t timeout_ms, uint32_t *out_generation)
{
    ESP_RETURN_ON_FALSE(network_lease_type_is_supported(type), ESP_ERR_INVALID_ARG, TAG, "网络产品租约类型无效");
    ESP_RETURN_ON_FALSE(out_generation != NULL, ESP_ERR_INVALID_ARG, TAG, "租约代次输出为空");
    *out_generation                 = 0U;

    network_manager_status_t status = { 0 };
    ESP_RETURN_ON_ERROR(network_manager_get_status_copy(&status), TAG, "读取网络状态失败");
    ESP_RETURN_ON_FALSE(status.state == NETWORK_STATE_ONLINE,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "当前网络状态不允许申请互斥网络产品租约");

    taskENTER_CRITICAL(&s_state_lock);
    reconcile_portal_transition_locked(&status);
    const bool generation_exhausted = s_next_lease_generation == 0U;
    const bool conflict             = s_light_sleep_suspended || s_active_lease_type != APP_NETWORK_LEASE_NONE
                                      || s_portal_transition_pending || s_ota_queued || s_ota_running;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(!generation_exhausted, ESP_ERR_INVALID_STATE, TAG, "网络产品租约代次已耗尽，需重启后恢复");
    ESP_RETURN_ON_FALSE(!conflict, ESP_ERR_INVALID_STATE, TAG, "Portal、OTA、轻睡眠或其他网络产品租约正在占用网络");

    return request_control_response_command(NETWORK_COMMAND_LEASE_ACQUIRE, type, 0U, timeout_ms, out_generation);
}

/**
 * @brief 同步释放一个类型和代次均匹配的互斥网络产品租约
 *
 * 无活动租约时保持幂等；存在租约时先做线程安全快照预检。Task 只认领尚未过期且仍归本请求
 * 所有的回执槽，认领后再次核对类型和代次，再把状态修改与完成回执绑定为同一结果。
 *
 * @param[in] type 待释放的具体产品租约类型
 * @param[in] generation 申请时获得的租约代次
 * @param[in] timeout_ms 命令被网络 Task 认领前的截止时间
 * @return ESP_OK 已释放或此前已释放；或参数、类型、代次、回执资源及超时错误码
 */
static esp_err_t release_network_lease(app_network_lease_type_t type, uint32_t generation, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(network_lease_type_is_supported(type), ESP_ERR_INVALID_ARG, TAG, "网络产品租约类型无效");
    ESP_RETURN_ON_FALSE(generation != 0U, ESP_ERR_INVALID_ARG, TAG, "租约代次无效");

    taskENTER_CRITICAL(&s_state_lock);
    const app_network_lease_type_t active_type       = s_active_lease_type;
    const uint32_t                 active_generation = s_active_lease_generation;
    taskEXIT_CRITICAL(&s_state_lock);
    if (active_type == APP_NETWORK_LEASE_NONE)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(type == active_type && generation == active_generation,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "租约类型或代次不匹配");

    return request_control_response_command(NETWORK_COMMAND_LEASE_RELEASE, type, generation, timeout_ms, NULL);
}

esp_err_t app_network_acquire_realtime_voice_lease(uint32_t timeout_ms, uint32_t *out_generation)
{
    return acquire_network_lease(APP_NETWORK_LEASE_REALTIME_VOICE, timeout_ms, out_generation);
}

esp_err_t app_network_release_realtime_voice_lease(uint32_t generation, uint32_t timeout_ms)
{
    return release_network_lease(APP_NETWORK_LEASE_REALTIME_VOICE, generation, timeout_ms);
}

esp_err_t app_network_acquire_web_file_lease(uint32_t timeout_ms, uint32_t *out_generation)
{
    return acquire_network_lease(APP_NETWORK_LEASE_WEB_FILE, timeout_ms, out_generation);
}

esp_err_t app_network_release_web_file_lease(uint32_t generation, uint32_t timeout_ms)
{
    return release_network_lease(APP_NETWORK_LEASE_WEB_FILE, generation, timeout_ms);
}

void app_network_get_lease_snapshot(app_network_lease_snapshot_t *out)
{
    if (out == NULL)
    {
        return;
    }
    taskENTER_CRITICAL(&s_state_lock);
    *out = (app_network_lease_snapshot_t) {
        .type       = s_active_lease_type,
        .active     = s_active_lease_type != APP_NETWORK_LEASE_NONE,
        .generation = s_active_lease_generation,
    };
    taskEXIT_CRITICAL(&s_state_lock);
}

esp_err_t app_network_start_portal(void)
{
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "网络任务未初始化");
    taskENTER_CRITICAL(&s_state_lock);
    const bool conflict =
        s_active_lease_type != APP_NETWORK_LEASE_NONE || s_light_sleep_suspended || s_ota_queued || s_ota_running;
    taskEXIT_CRITICAL(&s_state_lock);
    ESP_RETURN_ON_FALSE(!conflict, ESP_ERR_INVALID_STATE, TAG, "当前产品事务不允许进入配网");

    const network_command_t command = {
        .type = NETWORK_COMMAND_START_PORTAL,
    };
    return post_control_command(&command);
}
