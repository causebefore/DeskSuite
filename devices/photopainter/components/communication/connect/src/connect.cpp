/**
 * @file connect.cpp
 * 文件职责：实现 Wi-Fi STA 单次连接、原始链路事件上报和物理链路查询。
 * 主要依赖：ESP-IDF Wi-Fi、Event、Netif；connect_portal.c（Portal 资源操作）。
 * 调用方：network_manager。
 */
#include "connect.h"
#include "connect_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include "utils.h"

static const char *TAG = "connect";

/** @brief 收拢 connect 核心链路资源、回调和生命周期状态 */
struct ConnectRuntime
{
    bool                         wifi_started        = false;   /**< Wi-Fi Driver 是否已启动 */
    bool                         initialized         = false;   /**< connect 是否已初始化 */
    bool                         initializing        = false;   /**< 是否正在初始化 */
    bool                         deinitializing      = false;   /**< 是否正在反初始化 */
    bool                         cleanup_failed      = false;   /**< 是否存在未收敛清理错误 */
    esp_netif_t                 *sta_netif           = nullptr; /**< 默认 STA netif */
    esp_event_handler_instance_t wifi_event_instance = nullptr; /**< Wi-Fi 事件实例 */
    esp_event_handler_instance_t ip_event_instance   = nullptr; /**< IP 事件实例 */
    connect_callbacks_t          callbacks           = {};      /**< 借用的回调集合 */
    portMUX_TYPE                 callbacks_lock = portMUX_INITIALIZER_UNLOCKED; /**< 回调临界区锁 */
    portMUX_TYPE lifecycle_lock = portMUX_INITIALIZER_UNLOCKED; /**< 生命周期临界区锁 */
};

/** @brief connect 进程期 Runtime；资源释放只由显式 stop/deinit 驱动 */
static ConnectRuntime s_runtime;

/**
 * @brief 保留清理过程中遇到的第一个错误并记录后续错误
 *
 * @param[in,out] result 汇总结果
 * @param[in] error 当前步骤结果
 * @param[in] step 当前步骤中文名称
 */
static void preserve_cleanup_error(esp_err_t *result, esp_err_t error, const char *step)
{
    if (error == ESP_OK)
    {
        return;
    }
    ESP_LOGW(TAG, "%s失败：%s", step, esp_err_to_name(error));
    if (result != nullptr && *result == ESP_OK)
    {
        *result = error;
    }
}

/**
 * @brief 结束无需保留 Driver 的初始化失败流程
 *
 * @param[in] error 初始化错误码
 * @return 原始初始化错误码
 */
static esp_err_t finish_init_failure(esp_err_t error)
{
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.initializing = false;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    return error;
}

/**
 * @brief 标记初始化回滚未能完整释放 Wi-Fi 资源
 *
 * @param[in] cleanup_error 清理错误码
 * @return 清理错误码
 */
static esp_err_t finish_init_cleanup_failure(esp_err_t cleanup_error)
{
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.initialized    = true;
    s_runtime.initializing   = false;
    s_runtime.cleanup_failed = true;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    ESP_LOGE(TAG, "初始化失败后的 Wi-Fi 清理未完成：%s", esp_err_to_name(cleanup_error));
    return cleanup_error;
}

/**
 * @brief 回滚已经创建的 Wi-Fi Driver 和可选事件处理器
 *
 * @param[in] error 原始初始化错误码
 * @param[in] wifi_handler_registered 是否已经注册 Wi-Fi 事件处理器
 * @return 原始错误码；清理失败时返回清理错误码
 */
static esp_err_t rollback_wifi_init(esp_err_t error, bool wifi_handler_registered)
{
    if (wifi_handler_registered)
    {
        const esp_err_t cleanup_err =
            esp_event_handler_instance_unregister(WIFI_EVENT,
                                                  ESP_EVENT_ANY_ID,
                                                  s_runtime.wifi_event_instance);
        if (cleanup_err != ESP_OK)
        {
            return finish_init_cleanup_failure(cleanup_err);
        }
        s_runtime.wifi_event_instance = nullptr;
    }

    const esp_err_t cleanup_err = esp_wifi_deinit();
    if (cleanup_err != ESP_OK && cleanup_err != ESP_ERR_WIFI_NOT_INIT)
    {
        return finish_init_cleanup_failure(cleanup_err);
    }
    return finish_init_failure(error);
}

/**
 * @brief 在线程安全的临界区内复制当前回调集合
 *
 * @return 当前回调集合副本
 */
static connect_callbacks_t get_callbacks(void)
{
    taskENTER_CRITICAL(&s_runtime.callbacks_lock);
    const connect_callbacks_t callbacks = s_runtime.callbacks;
    taskEXIT_CRITICAL(&s_runtime.callbacks_lock);
    return callbacks;
}

/**
 * @brief 按给定长度复制原始 SSID 并保证字符串以空字符结尾
 *
 * @param[out] out 输出字符串
 * @param[in] out_size 输出缓冲区大小
 * @param[in] ssid 原始 SSID 字节
 * @param[in] ssid_len 原始 SSID 长度
 */
static void copy_ssid(char *out, size_t out_size, const uint8_t *ssid, size_t ssid_len)
{
    if (out == nullptr || out_size == 0)
    {
        return;
    }
    out[0] = '\0';
    if (ssid == nullptr)
    {
        return;
    }

    const size_t copy_len = ssid_len < out_size - 1 ? ssid_len : out_size - 1;
    memcpy(out, ssid, copy_len);
    out[copy_len] = '\0';
}

/**
 * @brief 向调用方同步上报一个不可变链路事件
 *
 * @param[in] event 链路事件
 */
static void notify_link_event(const connect_link_event_t *event)
{
    const connect_callbacks_t callbacks = get_callbacks();
    if (callbacks.on_link_event != nullptr)
    {
        callbacks.on_link_event(event, callbacks.ctx);
    }
}

/**
 * @brief 将 Portal 表单提交转交给已注册回调
 *
 * @param[in] submission Portal 表单提交数据
 * @return ESP_OK 调用方已接收；ESP_ERR_INVALID_STATE 未注册回调；其他值由回调返回
 */
esp_err_t connect_internal_submit_credentials(const connect_portal_submission_t *submission)
{
    if (submission == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const connect_callbacks_t callbacks = get_callbacks();
    if (callbacks.on_credentials_submitted == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return callbacks.on_credentials_submitted(submission, callbacks.ctx);
}

/**
 * @brief 将 Portal 显式用户活动转交给已注册回调
 *
 * @return ESP_OK 调用方已接收；ESP_ERR_INVALID_STATE 未注册回调；其他值由回调返回
 */
esp_err_t connect_internal_notify_portal_activity(void)
{
    const connect_callbacks_t callbacks = get_callbacks();
    if (callbacks.on_portal_activity == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return callbacks.on_portal_activity(callbacks.ctx);
}

/**
 * @brief 确保 Wi-Fi 驱动已启动
 *
 * @return ESP_OK 已启动或原本已启动；其他值表示驱动启动失败
 */
esp_err_t connect_internal_wifi_start_once(void)
{
    if (!connect_internal_accepts_operations())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.wifi_started)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "启动 Wi-Fi 失败");
    s_runtime.wifi_started = true;
    return ESP_OK;
}

/**
 * @brief 判断 connect 当前是否允许启动新的链路操作
 *
 * @return true 已初始化且未进入反初始化；false 当前拒绝新操作
 */
bool connect_internal_accepts_operations(void)
{
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    const bool accepts =
        s_runtime.initialized && !s_runtime.deinitializing && !s_runtime.cleanup_failed;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    return accepts;
}

/**
 * @brief 确保默认 STA netif 已在 Wi-Fi 启动前创建
 *
 * @return ESP_OK 已存在或创建成功；ESP_ERR_NO_MEM 创建失败
 */
esp_err_t connect_internal_ensure_sta_netif(void)
{
    if (s_runtime.sta_netif == nullptr)
    {
        s_runtime.sta_netif = esp_netif_create_default_wifi_sta();
    }
    return s_runtime.sta_netif != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

/**
 * @brief 将 GOT_IP 事件转换为只读链路事件并上报
 *
 * @param[in] event_data ESP-IDF GOT_IP 事件数据
 */
static void handle_got_ip(const void *event_data)
{
    connect_link_event_t event = {};
    event.type                 = CONNECT_LINK_EVENT_GOT_IP;
    event.link.associated      = true;
    event.link.has_ipv4        = true;

    const auto *ip_event       = static_cast<const ip_event_got_ip_t *>(event_data);
    if (ip_event != nullptr)
    {
        (void) snprintf(event.link.ip, sizeof(event.link.ip), IPSTR, IP2STR(&ip_event->ip_info.ip));
    }

    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    {
        copy_ssid(event.link.ssid, sizeof(event.link.ssid), ap.ssid, sizeof(ap.ssid));
        event.link.rssi_dbm = ap.rssi;
    }
    notify_link_event(&event);
}

/**
 * @brief 将 DISCONNECTED 事件转换为只读链路事件并上报
 *
 * @param[in] event_data ESP-IDF DISCONNECTED 事件数据
 */
static void handle_disconnected(const void *event_data)
{
    connect_link_event_t event = {};
    event.type                 = CONNECT_LINK_EVENT_DISCONNECTED;
    const auto *disconnected   = static_cast<const wifi_event_sta_disconnected_t *>(event_data);
    if (disconnected != nullptr)
    {
        copy_ssid(event.link.ssid,
                  sizeof(event.link.ssid),
                  disconnected->ssid,
                  disconnected->ssid_len);
        event.link.rssi_dbm     = disconnected->rssi;
        event.disconnect_reason = disconnected->reason;
    }

    notify_link_event(&event);
}

/**
 * @brief 将 ESP-IDF Wi-Fi/IP 事件转换为底层链路事实事件
 *
 * @param[in] arg 未使用参数
 * @param[in] event_base 事件基类
 * @param[in] event_id 事件编号
 * @param[in] event_data 事件数据
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void) arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        handle_disconnected(event_data);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        handle_got_ip(event_data);
    }
}

/**
 * @brief 设置 connect 异步事件回调
 *
 * @param[in] callbacks 回调集合；nullptr 表示清除
 */
void connect_set_callbacks_borrow(const connect_callbacks_t *callbacks)
{
    taskENTER_CRITICAL(&s_runtime.callbacks_lock);
    if (callbacks != nullptr)
    {
        s_runtime.callbacks = *callbacks;
    }
    else
    {
        memset(&s_runtime.callbacks, 0, sizeof(s_runtime.callbacks));
    }
    taskEXIT_CRITICAL(&s_runtime.callbacks_lock);
}

/**
 * @brief 初始化 Wi-Fi 链路能力
 *
 * @return ESP_OK 成功；其他值表示 ESP-IDF 初始化失败
 */
esp_err_t connect_init(void)
{
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    if (s_runtime.initialized)
    {
        const bool cleanup_failed = s_runtime.cleanup_failed;
        taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
        return cleanup_failed ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    if (s_runtime.initializing || s_runtime.deinitializing)
    {
        taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.initializing = true;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "初始化 netif 失败：%s", esp_err_to_name(err));
        return finish_init_failure(err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "创建事件循环失败：%s", esp_err_to_name(err));
        return finish_init_failure(err);
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err                         = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 Wi-Fi 失败：%s", esp_err_to_name(err));
        return finish_init_failure(err);
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              wifi_event_handler,
                                              nullptr,
                                              &s_runtime.wifi_event_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "注册 Wi-Fi 事件处理函数失败：%s", esp_err_to_name(err));
        return rollback_wifi_init(err, false);
    }
    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler,
                                              nullptr,
                                              &s_runtime.ip_event_instance);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "注册 IP 事件处理函数失败：%s", esp_err_to_name(err));
        return rollback_wifi_init(err, true);
    }

    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.initialized    = true;
    s_runtime.initializing   = false;
    s_runtime.cleanup_failed = false;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    return ESP_OK;
}

/**
 * @brief 复制指定配置并请求启动一次 STA 连接
 *
 * @param[in] config STA 连接参数
 * @return ESP_OK 已发起连接；其他值表示参数或驱动操作失败
 */
esp_err_t connect_request_start_station_copy(const connect_sta_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != nullptr && config->ssid[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "STA 配置无效");
    ESP_RETURN_ON_ERROR(connect_init(), TAG, "初始化连接能力失败");
    ESP_RETURN_ON_FALSE(connect_internal_accepts_operations(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "连接组件正在停止");

    ESP_RETURN_ON_ERROR(connect_internal_ensure_sta_netif(), TAG, "创建 STA netif 失败");

    const esp_err_t dns_stop_err    = connect_internal_portal_dns_stop();
    const esp_err_t portal_stop_err = connect_internal_stop_config_portal();
    ESP_RETURN_ON_ERROR(dns_stop_err, TAG, "停止配网 DNS 失败");
    ESP_RETURN_ON_ERROR(portal_stop_err, TAG, "停止配网 Portal 失败");

    wifi_config_t wifi_config = {};
    utils_copy_string(reinterpret_cast<char *>(wifi_config.sta.ssid),
                      sizeof(wifi_config.sta.ssid),
                      config->ssid);
    utils_copy_string(reinterpret_cast<char *>(wifi_config.sta.password),
                      sizeof(wifi_config.sta.password),
                      config->password);
    wifi_config.sta.threshold.authmode =
        config->password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置 STA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "设置 STA 配置失败");
    ESP_RETURN_ON_ERROR(connect_internal_wifi_start_once(), TAG, "启动 Wi-Fi 失败");

    const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ps_err != ESP_OK)
    {
        ESP_LOGW(TAG, "设置 Wi-Fi 省电失败：%s", esp_err_to_name(ps_err));
    }
    return esp_wifi_connect();
}

/**
 * @brief 保留配网 Portal 并发起一次 STA 候选连接
 *
 * @param[in] config STA 候选连接参数
 * @return ESP_OK 已发起连接；其他值表示参数、Portal 状态或驱动操作失败
 */
esp_err_t connect_request_start_station_with_portal_copy(const connect_sta_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != nullptr && config->ssid[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "STA 候选配置无效");
    ESP_RETURN_ON_ERROR(connect_init(), TAG, "初始化连接能力失败");
    ESP_RETURN_ON_FALSE(connect_internal_accepts_operations(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "连接组件正在停止");
    ESP_RETURN_ON_ERROR(connect_internal_ensure_sta_netif(), TAG, "创建 STA netif 失败");
    ESP_RETURN_ON_ERROR(connect_internal_portal_scan_stop(), TAG, "停止配网扫描失败");

    wifi_config_t wifi_config = {};
    utils_copy_string(reinterpret_cast<char *>(wifi_config.sta.ssid),
                      sizeof(wifi_config.sta.ssid),
                      config->ssid);
    utils_copy_string(reinterpret_cast<char *>(wifi_config.sta.password),
                      sizeof(wifi_config.sta.password),
                      config->password);
    wifi_config.sta.threshold.authmode =
        config->password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "保持配网 APSTA 模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config),
                        TAG,
                        "设置 STA 候选配置失败");
    ESP_RETURN_ON_ERROR(connect_internal_wifi_start_once(), TAG, "启动 Wi-Fi 失败");

    const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ps_err != ESP_OK)
    {
        ESP_LOGW(TAG, "设置 Wi-Fi 省电失败：%s", esp_err_to_name(ps_err));
    }
    return esp_wifi_connect();
}

esp_err_t connect_disconnect_station_keep_portal(void)
{
    if (!connect_internal_accepts_operations())
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = esp_wifi_disconnect();
    return error == ESP_ERR_WIFI_NOT_CONNECT ? ESP_OK : error;
}

esp_err_t connect_complete_portal_station(void)
{
    if (!connect_internal_accepts_operations())
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(connect_internal_portal_dns_stop(), TAG, "停止配网 DNS 失败");
    ESP_RETURN_ON_ERROR(connect_internal_stop_config_portal(), TAG, "停止配网 Portal 失败");
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

/**
 * @brief 同步复制当前 STA 物理链路和 IPv4 快照
 *
 * @param[out] out 当前链路信息
 * @return ESP_OK 查询完成；其他值表示参数、初始化或底层查询失败
 */
esp_err_t connect_get_link_snapshot_copy(connect_link_info_t *out)
{
    if (out == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!connect_internal_accepts_operations())
    {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_ap_record_t ap     = {};
    const esp_err_t  ap_err = esp_wifi_sta_get_ap_info(&ap);
    if (ap_err == ESP_ERR_WIFI_NOT_CONNECT || ap_err == ESP_ERR_WIFI_NOT_STARTED
        || ap_err == ESP_ERR_WIFI_CONN)
    {
        return ESP_OK;
    }
    if (ap_err != ESP_OK)
    {
        return ap_err;
    }

    out->associated = true;
    out->rssi_dbm   = ap.rssi;
    copy_ssid(out->ssid, sizeof(out->ssid), ap.ssid, sizeof(ap.ssid));
    if (s_runtime.sta_netif == nullptr)
    {
        return ESP_OK;
    }

    esp_netif_ip_info_t ip_info = {};
    const esp_err_t     ip_err  = esp_netif_get_ip_info(s_runtime.sta_netif, &ip_info);
    if (ip_err != ESP_OK)
    {
        return ip_err;
    }
    if (ip_info.ip.addr != 0)
    {
        out->has_ipv4 = true;
        (void) snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip_info.ip));
    }
    return ESP_OK;
}

/**
 * @brief 停止 connect 持有的 Portal 和 Wi-Fi 资源
 *
 * @return ESP_OK 成功；其他值表示停止 Wi-Fi 失败
 */
esp_err_t connect_stop(void)
{
    connect_set_callbacks_borrow(nullptr);
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    const bool initialized = s_runtime.initialized;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    if (!initialized)
    {
        return ESP_OK;
    }

    esp_err_t result = connect_internal_portal_dns_stop();
    preserve_cleanup_error(&result, connect_internal_stop_config_portal(), "停止配网 Portal");
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Portal 私有任务尚未安全退出，保留 Wi-Fi Driver 以便故障收敛");
        return result;
    }

    if (s_runtime.wifi_started)
    {
        (void) esp_wifi_disconnect();
        const esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED)
        {
            preserve_cleanup_error(&result, err, "停止 Wi-Fi");
        }
        else
        {
            s_runtime.wifi_started = false;
        }
    }

    ESP_LOGI(TAG, "Wi-Fi 已停止");
    return result;
}

/**
 * @brief 记录反初始化未完全收敛并保持不可重启状态
 *
 * @param[in] error 清理错误码
 * @return 原始清理错误码
 */
static esp_err_t finish_deinit_cleanup_failure(esp_err_t error)
{
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.deinitializing = false;
    s_runtime.cleanup_failed = true;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    ESP_LOGE(TAG, "Wi-Fi 资源清理未完成，保持不可重启状态：%s", esp_err_to_name(error));
    return error;
}

/**
 * @brief 同步反初始化 connect 及其独占的 Wi-Fi 资源
 *
 * @return ESP_OK 已完成清理或原本未初始化；其他值表示至少一个清理步骤失败
 */
esp_err_t connect_deinit(void)
{
    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    if (!s_runtime.initialized)
    {
        taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
        connect_set_callbacks_borrow(nullptr);
        return ESP_OK;
    }
    if (s_runtime.initializing || s_runtime.deinitializing)
    {
        taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.deinitializing = true;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);

    esp_err_t result = connect_stop();
    if (result != ESP_OK)
    {
        return finish_deinit_cleanup_failure(result);
    }

    if (s_runtime.ip_event_instance != nullptr)
    {
        const esp_err_t err = esp_event_handler_instance_unregister(IP_EVENT,
                                                                    IP_EVENT_STA_GOT_IP,
                                                                    s_runtime.ip_event_instance);
        preserve_cleanup_error(&result, err, "注销 IP 事件处理器");
        if (err == ESP_OK)
        {
            s_runtime.ip_event_instance = nullptr;
        }
    }
    if (s_runtime.wifi_event_instance != nullptr)
    {
        const esp_err_t err = esp_event_handler_instance_unregister(WIFI_EVENT,
                                                                    ESP_EVENT_ANY_ID,
                                                                    s_runtime.wifi_event_instance);
        preserve_cleanup_error(&result, err, "注销 Wi-Fi 事件处理器");
        if (err == ESP_OK)
        {
            s_runtime.wifi_event_instance = nullptr;
        }
    }
    if (result != ESP_OK)
    {
        return finish_deinit_cleanup_failure(result);
    }

    if (s_runtime.sta_netif != nullptr)
    {
        esp_netif_destroy_default_wifi(s_runtime.sta_netif);
        s_runtime.sta_netif = nullptr;
    }
    connect_internal_destroy_ap_netif();

    const esp_err_t deinit_err = esp_wifi_deinit();
    if (deinit_err != ESP_OK && deinit_err != ESP_ERR_WIFI_NOT_INIT)
    {
        preserve_cleanup_error(&result, deinit_err, "反初始化 Wi-Fi Driver");
        return finish_deinit_cleanup_failure(result);
    }

    taskENTER_CRITICAL(&s_runtime.lifecycle_lock);
    s_runtime.wifi_started   = false;
    s_runtime.initialized    = false;
    s_runtime.deinitializing = false;
    s_runtime.cleanup_failed = false;
    taskEXIT_CRITICAL(&s_runtime.lifecycle_lock);
    ESP_LOGI(TAG, "Wi-Fi Driver 与会话资源已释放");
    return result;
}
