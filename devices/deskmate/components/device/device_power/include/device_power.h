/**
 * @file device_power.h
 * @brief 不暴露板级 GPIO 的设备轻睡眠能力
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 一次设备级轻睡眠事务的唤醒结果 */
    typedef struct
    {
        bool left_button;  /**< 左键导致唤醒 */
        bool right_button; /**< 右键导致唤醒 */
        bool timer;        /**< ESP32 内部 Timer 导致唤醒 */
    } device_power_wakeup_result_t;

    /**
     * @brief 初始化设备轻睡眠能力
     *
     * 在系统启动早期一次性配置全局睡眠参数。重复初始化返回 ESP_ERR_INVALID_STATE。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 重复初始化；或底层错误码
     */
    esp_err_t device_power_init(void);

    /**
     * @brief 按编译配置执行一次完整轻睡眠事务
     *
     * 本函数使用左右按键和 ESP32 内部 Timer 执行一次同步 Light-sleep 事务。调用期间不会
     * 复位芯片；唤醒后继续执行原 Application 生命周期。
     *
     * @param[in] timer_wakeup_ms 维护唤醒间隔，单位毫秒，必须大于 0
     * @param[out] out_result 本次事务的唤醒结果，仅在 ESP_OK 时有效
     * @return ESP_OK 已唤醒且清理完成；ESP_ERR_INVALID_ARG 参数无效；
     *         ESP_ERR_INVALID_STATE 按键尚未释放；或底层错误码
     */
    esp_err_t device_power_enter_light_sleep(uint32_t timer_wakeup_ms, device_power_wakeup_result_t *out_result);

#ifdef __cplusplus
}
#endif
