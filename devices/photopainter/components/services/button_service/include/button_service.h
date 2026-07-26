/**
 * @file button_service.h
 * @brief 按键持续扫描与事件投递 Service
 */
#pragma once

#include "device_button.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Service 对上发布的按键事件回调
     *
     * 回调在 System 周期定时执行上下文运行，必须快速返回；耗时业务应复制参数后投递到自己
     * 的队列。回调不得重入 button_service_* 控制 API。
     *
     * @param[in] button 按键标识
     * @param[in] event 已完成去抖和手势识别的设备事件
     * @param[in] click_count 连击次数，仅点击类事件有意义
     * @param[in] context 注册时传入的上下文
     */
    typedef void (*button_service_event_cb_t)(device_button_id_t    button,
                                              device_button_event_t event, uint8_t click_count,
                                              void *context);

    /**
     * @brief 初始化按键持续扫描 Service
     *
     * 调用前必须已成功初始化 device_button。本函数注册 Device 快速事件入口并创建尚未运行的
     * System 周期定时器，不启动扫描。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化或 Device 尚未初始化；
     *         ESP_ERR_NO_MEM 内存不足；或 System、Device 错误码
     */
    esp_err_t button_service_init(void);

    /**
     * @brief 设置或清除 Service 按键事件回调
     *
     * 仅允许在扫描停止时调用。callback 和 context 的借用持续到下一次设置、显式传入 NULL
     * 清除或 button_service_deinit()，以先发生者为准。button_service_stop() 只停止扫描，
     * 不结束回调借用；再次启动后继续使用原回调与 context。
     *
     * @param[in] callback 回调；NULL 表示清除
     * @param[in] context 回调上下文
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或正在运行
     */
    esp_err_t button_service_set_event_callback_borrow(button_service_event_cb_t callback,
                                                       void                     *context);

    /**
     * @brief 启动 10 ms 周期按键扫描
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或已经运行；
     *         或 System 执行环境错误码
     */
    esp_err_t button_service_start(void);

    /**
     * @brief 同步停止周期扫描并保留上层回调借用
     *
     * 返回 ESP_OK 后不再安排新的周期扫描；调用时已经开始的定时回调仍可能在原执行上下文中
     * 完成。已注册的 callback 和 context 保持不变，调用方可以直接再次调用
     * button_service_start() 恢复扫描。需要结束借用时，应在停止成功后显式调用
     * button_service_set_event_callback_borrow(NULL, NULL) 或继续执行 button_service_deinit()。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或未运行；
     *         或 System 执行环境错误码
     */
    esp_err_t button_service_stop(void);

    /**
     * @brief 释放按键 Service 定时器与 Device 回调借用
     *
     * 必须先成功停止扫描。本函数不反初始化 device_button，由 Composition Root 单独释放。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或仍在运行；
     *         或 System、Device 错误码
     */
    esp_err_t button_service_deinit(void);

#ifdef __cplusplus
}
#endif
