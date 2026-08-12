/**
 * @file provisioning_app.h
 * @brief 网络可用前的配网判断与墨水屏二维码页面编排接口
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 检查当前网络配置是否包含可用 Hub 上下文
     *
     * 回调在 provisioning_app 调用者 Task 上下文同步执行，不得启动网络或刷新显示。
     *
     * @param[in] context 注册时传入的借用上下文
     * @return ESP_OK Hub 配置可用；ESP_ERR_NOT_FOUND Hub 尚未配置；或其他读取、校验错误码
     */
    typedef esp_err_t (*provisioning_app_backend_ready_cb_t)(void *context);

    /** @brief 配网用例启动上下文 */
    typedef struct
    {
        bool woken_by_button; /**< 本次是否由任意按键从深睡唤醒 */
        bool woken_by_timer;  /**< 本次是否由内部定时器从深睡唤醒 */
        bool force_portal;    /**< true 表示忽略旧连接的瞬时 ONLINE 并恢复现有 Portal */
        provisioning_app_backend_ready_cb_t backend_ready_callback; /**< Hub 配置检查回调 */
        void *backend_ready_context; /**< Hub 配置检查回调上下文 */
    } provisioning_app_config_t;

    /**
     * @brief 运行配网用例，直到网络可用或出现不可收敛错误
     *
     * 调用前必须已初始化 system_storage、device_display、network_manager、device_button 和
     * 已停止的 button_service，且当前没有活动网络会话。函数会临时接管按键扫描；网络或 Hub
     * 不可用时先显示英文提示，只有物理中键长按三秒才请求既有 Portal 并显示既有二维码。
     *
     * Portal 使用三分钟无交互窗口；打开页面、真实页面操作、提交配置和候选连接失败会重新
     * 计时，自动扫描与状态轮询不会续期。窗口到期后保留二维码并进入仅按键可唤醒的深睡。
     * 定时唤醒只执行有界重连，失败后按 1/5/15 分钟及后续整点退避再次深睡；按键唤醒后的连接
     * 失败进入 180 秒提示窗口，仍需再次长按中键才进入 Portal。深睡成功时本函数不返回。
     * 返回 ESP_OK 时网络会话保持运行。不得从 network_manager 变化通知回调中调用。
     *
     * @param[in] config 本次唤醒来源；函数只在调用期间借用
     * @return ESP_OK 网络已获得可用 IPv4；ESP_ERR_INVALID_STATE 前置生命周期不满足；
     *         或网络、二维码生成、显示刷新、深睡准备和清理错误码
     */
    esp_err_t provisioning_app_run_until_online(const provisioning_app_config_t *config);

#ifdef __cplusplus
}
#endif
