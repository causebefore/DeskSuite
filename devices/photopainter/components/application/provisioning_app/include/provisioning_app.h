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

    /** @brief 配网用例启动上下文 */
    typedef struct
    {
        bool woken_by_button; /**< 本次是否由任意按键从深睡唤醒 */
        bool woken_by_timer;  /**< 本次是否由内部定时器从深睡唤醒 */
    } provisioning_app_config_t;

    /**
     * @brief 运行配网用例，直到网络可用或出现不可收敛错误
     *
     * 调用前必须已初始化 system_storage、device_display 和 network_manager，且当前没有活动的
     * network_manager 会话。函数会启动一轮网络会话并同步等待状态变化；无有效配置时请求
     * Portal，在调用者 Task 中把配网二维码刷新到墨水屏。若本轮曾显示二维码，联网后会将其
     * 清为白色再返回，随后调用方可以进入正常页面流程。
     *
     * Portal 使用三分钟无交互窗口；打开页面、真实页面操作、提交配置和候选连接失败会重新
     * 计时，自动扫描与状态轮询不会续期。窗口到期后保留二维码并进入仅按键可唤醒的深睡。
     * 定时唤醒只执行有界重连，失败后按 1/5/15 分钟及后续一小时退避再次深睡；按键唤醒后的连接
     * 失败进入 Portal 维修窗口。深睡成功时本函数不返回，准备失败时返回对应错误。
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
