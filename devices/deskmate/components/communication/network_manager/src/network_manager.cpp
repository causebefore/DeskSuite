/**
 * @file network_manager.cpp
 * @brief 实现网络管理器公共 API、状态元数据和回调同步
 */
#include "network_manager.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager_internal.h"

/** @brief 日志标签 */
static const char *TAG = "network_manager";

/** @brief 收拢 Network Manager 公共门面的并发状态 */
struct NetworkManagerPublicState
{
    bool                        initialized         = false;                        /**< 公共 API 是否已初始化 */
    bool                        has_saved_config    = false;                        /**< active 配置是否已持久化 */
    network_manager_status_t    status              = {};                           /**< 最近发布的状态元数据 */
    connect_portal_info_t       portal_info         = {};                           /**< 按需读取的 Portal 信息 */
    network_manager_notify_cb_t notify_callback     = nullptr;                      /**< 借用的变化通知回调 */
    void                       *notify_callback_ctx = nullptr;                      /**< 借用的回调上下文 */
    portMUX_TYPE                lock                = portMUX_INITIALIZER_UNLOCKED; /**< 公共状态临界区锁 */
};

/** @brief Network Manager 公共门面状态 */
static NetworkManagerPublicState s_public_state;

/**
 * @brief 在短临界区内读取初始化标记
 *
 * @return true 已初始化；false 尚未初始化
 */
static bool network_manager_is_initialized(void)
{
    taskENTER_CRITICAL(&s_public_state.lock);
    const bool initialized = s_public_state.initialized;
    taskEXIT_CRITICAL(&s_public_state.lock);
    return initialized;
}

/**
 * @brief 发布不可变网络状态元数据并在锁外通知当前订阅者
 *
 * @param[in] status 待复制状态元数据
 * @param[in] has_saved_config 当前 active 配置是否已经持久化
 */
void network_manager_internal_publish_status_copy(const network_manager_status_t *status, bool has_saved_config)
{
    if (status == nullptr)
    {
        ESP_LOGE(TAG, "发布网络状态失败：状态指针为空");
        return;
    }

    const network_manager_status_t published = *status;
    network_manager_notify_cb_t    callback;
    void                          *callback_ctx;

    taskENTER_CRITICAL(&s_public_state.lock);
    s_public_state.status           = published;
    s_public_state.has_saved_config = has_saved_config;
    callback                        = s_public_state.notify_callback;
    callback_ctx                    = s_public_state.notify_callback_ctx;
    taskEXIT_CRITICAL(&s_public_state.lock);

    if (callback != nullptr)
    {
        callback(callback_ctx);
    }
}

/** @brief 更新按需读取的 Portal 展示信息 */
void network_manager_internal_set_portal_info_copy(const connect_portal_info_t *info)
{
    if (info == nullptr)
    {
        ESP_LOGE(TAG, "更新 Portal 信息失败：信息指针为空");
        return;
    }
    taskENTER_CRITICAL(&s_public_state.lock);
    s_public_state.portal_info = *info;
    taskEXIT_CRITICAL(&s_public_state.lock);
}

/**
 * @brief 初始化 Network Manager 长期队列与同步资源
 *
 * @return ESP_OK 成功；或内部资源初始化错误码
 */
esp_err_t network_manager_init_borrow(const network_manager_config_store_t *config_store)
{
    ESP_RETURN_ON_FALSE(config_store != nullptr && config_store->load_config_copy != nullptr
                            && config_store->save_config_borrow != nullptr && config_store->erase_config != nullptr,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "网络配置持久化回调不完整");
    if (network_manager_is_initialized())
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(network_manager_internal_task_init_borrow(config_store), TAG, "初始化网络管理任务失败");

    taskENTER_CRITICAL(&s_public_state.lock);
    s_public_state.initialized = true;
    taskEXIT_CRITICAL(&s_public_state.lock);
    return ESP_OK;
}

/**
 * @brief 启动一轮新的有界网络管理会话
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或生命周期不允许；或资源错误码
 */
esp_err_t network_manager_start(void)
{
    ESP_RETURN_ON_FALSE(network_manager_is_initialized(), ESP_ERR_INVALID_STATE, TAG, "网络管理器尚未初始化");
    ESP_RETURN_ON_ERROR(network_manager_internal_task_start(), TAG, "启动网络会话失败");
    return ESP_OK;
}

/**
 * @brief 同步停止网络会话并释放 Wi-Fi Driver
 *
 * @return ESP_OK 已完全停止；ESP_ERR_INVALID_STATE 生命周期不允许；其他值表示清理失败
 */
esp_err_t network_manager_stop(void)
{
    ESP_RETURN_ON_FALSE(network_manager_is_initialized(), ESP_ERR_INVALID_STATE, TAG, "网络管理器尚未初始化");
    const esp_err_t err = network_manager_internal_task_stop();
    if (!network_manager_internal_task_has_active_task())
    {
        taskENTER_CRITICAL(&s_public_state.lock);
        s_public_state.notify_callback     = nullptr;
        s_public_state.notify_callback_ctx = nullptr;
        taskEXIT_CRITICAL(&s_public_state.lock);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "停止网络会话失败");
    return ESP_OK;
}

/**
 * @brief 复制当前网络状态元数据
 *
 * @param[out] out_status 状态输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效
 */
esp_err_t network_manager_get_status_copy(network_manager_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != nullptr, ESP_ERR_INVALID_ARG, TAG, "状态输出指针为空");

    taskENTER_CRITICAL(&s_public_state.lock);
    *out_status = s_public_state.status;
    taskEXIT_CRITICAL(&s_public_state.lock);
    return ESP_OK;
}

/** @brief 复制当前 Portal 展示信息 */
esp_err_t network_manager_get_portal_info_copy(connect_portal_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(out_info != nullptr, ESP_ERR_INVALID_ARG, TAG, "Portal 输出指针为空");

    *out_info = {};
    taskENTER_CRITICAL(&s_public_state.lock);
    const bool available = (s_public_state.status.state == NETWORK_STATE_PROVISIONING
                            || s_public_state.status.state == NETWORK_STATE_VALIDATING)
                           && s_public_state.portal_info.active;
    if (available)
    {
        *out_info = s_public_state.portal_info;
    }
    taskEXIT_CRITICAL(&s_public_state.lock);
    return ESP_OK;
}

/** @brief 查询当前 active 网络配置是否已经持久化 */
esp_err_t network_manager_has_saved_config(bool *out_has_saved_config)
{
    ESP_RETURN_ON_FALSE(out_has_saved_config != nullptr, ESP_ERR_INVALID_ARG, TAG, "配置状态输出指针为空");
    taskENTER_CRITICAL(&s_public_state.lock);
    *out_has_saved_config = s_public_state.has_saved_config;
    taskEXIT_CRITICAL(&s_public_state.lock);
    return ESP_OK;
}

/**
 * @brief 设置会话期间借用的网络变化通知回调
 *
 * @param[in] callback 回调，可为 nullptr
 * @param[in] ctx 借用上下文，可为 nullptr
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t network_manager_set_notify_callback_borrow(network_manager_notify_cb_t callback, void *ctx)
{
    ESP_RETURN_ON_FALSE(network_manager_is_initialized(), ESP_ERR_INVALID_STATE, TAG, "网络管理器尚未初始化");

    taskENTER_CRITICAL(&s_public_state.lock);
    s_public_state.notify_callback     = callback;
    s_public_state.notify_callback_ctx = ctx;
    taskEXIT_CRITICAL(&s_public_state.lock);
    return ESP_OK;
}

/**
 * @brief 请求当前会话进入配网 Portal
 *
 * @return ESP_OK 命令已入队；或生命周期、队列错误码
 */
esp_err_t network_manager_request_start_portal(void)
{
    ESP_RETURN_ON_ERROR(network_manager_internal_request_start_portal(), TAG, "提交启动配网 Portal 命令失败");
    return ESP_OK;
}

/**
 * @brief 请求清除配置并进入配网 Portal
 *
 * @return ESP_OK 命令已入队；或生命周期、队列错误码
 */
esp_err_t network_manager_request_forget_and_start_portal(void)
{
    ESP_RETURN_ON_ERROR(network_manager_internal_request_forget_and_start_portal(),
                        TAG,
                        "提交清除配置并启动 Portal 命令失败");
    return ESP_OK;
}
