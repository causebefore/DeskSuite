/**
 * @file button_service.h
 * @brief 按需推进按键状态机并向 Application 转发稳定事件
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_button.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 按键扫描 Service 配置 */
    typedef struct
    {
        uint32_t scan_period_ms; /*!< 活动窗口状态机推进间隔，单位毫秒 */
    } button_service_config_t;

    /** @brief 一次 Light Sleep 返回时锁存的按键唤醒事实 */
    typedef struct
    {
        bool left_button;  /**< EXT1 表明左键曾拉低 */
        bool right_button; /**< EXT1 表明右键曾拉低 */
    } button_service_wakeup_info_t;

    /**
     * @brief 按键事件回调
     *
     * 回调在 ESP Timer Task 上下文同步执行，必须快速返回且不得阻塞。
     *
     * @param[in] event 已稳定的产品按键事件
     * @param[in] timestamp_ms 事件产生时的单调时间，单位毫秒
     * @param[in] context 长期借用的调用方上下文
     */
    typedef void (*button_service_event_cb_t)(device_button_event_t event, uint32_t timestamp_ms, void *context);

    /**
     * @brief 创建尚未运行的按键扫描 Service
     *
     * 本函数不初始化 `device_button`，其生命周期由 Composition Root 管理。
     *
     * @param[in] config 按键状态机推进配置
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 配置无效；ESP_ERR_INVALID_STATE 已初始化；
     *         ESP_ERR_NO_MEM 无法创建同步信号；或 ESP Timer 错误码
     */
    esp_err_t button_service_init(const button_service_config_t *config);

    /**
     * @brief 在停止状态设置长期借用的事件回调
     *
     * 传入 NULL 可清除回调。Service 运行期间不允许替换回调。
     *
     * @param[in] callback 事件回调；NULL 表示清除
     * @param[in] context 长期借用的回调上下文；清除时忽略
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE Service 未初始化或未完全停止
     */
    esp_err_t button_service_set_event_callback_borrow(button_service_event_cb_t callback, void *context);

    /**
     * @brief 启动边沿监听并安排一次初始扫描
     * @return ESP_OK 已启动；ESP_ERR_INVALID_STATE 生命周期状态不允许；或底层调度错误码
     */
    esp_err_t button_service_start(void);

    /**
     * @brief 按值提交 Light Sleep 按键唤醒事实并安排扫描
     *
     * ESP_OK 只表示请求已复制；最终产品事件仍通过事件回调返回。
     *
     * @param[in] wakeup 至少包含一个按键位的唤醒事实
     * @return ESP_OK 已接受；ESP_ERR_INVALID_ARG 参数为空或无按键事实；
     *         ESP_ERR_INVALID_STATE Service 未运行；或 Timer 调度错误码
     */
    esp_err_t button_service_request_light_sleep_wakeup_copy(const button_service_wakeup_info_t *wakeup);

    /**
     * @brief 同步停止边沿监听和扫描
     *
     * ESP_OK 返回后保证没有 GPIO 活动回调、扫描回调或上层事件回调仍在执行。
     * 超时时保持 STOPPING，调用方可再次调用 stop() 收敛；start() 和 deinit() 会被拒绝。
     *
     * @param[in] timeout_ms 等待在途回调退出的超时，必须大于 0
     * @return ESP_OK 已完全停止；ESP_ERR_TIMEOUT 尚有在途回调；或状态/底层错误码
     */
    esp_err_t button_service_stop(uint32_t timeout_ms);

    /**
     * @brief 销毁已停止的 Service
     *
     * 本函数不释放 `device_button`。
     *
     * @return ESP_OK 已销毁；ESP_ERR_INVALID_STATE Service 未初始化或未完全停止；
     *         或 ESP Timer 错误码
     */
    esp_err_t button_service_deinit(void);

#ifdef __cplusplus
}
#endif
