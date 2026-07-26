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

    /** @brief 一次轻睡眠返回时锁存的设备级唤醒来源 */
    typedef struct
    {
        bool left_button;  /**< 左键导致唤醒 */
        bool right_button; /**< 右键导致唤醒 */
        bool timer;        /**< ESP32 内部 Timer 导致唤醒 */
    } device_power_wakeup_info_t;

    /**
     * @brief 以左右按键和内部 Timer 为唤醒源执行一次完整轻睡眠事务
     *
     * 本函数同步完成按键释放检查、EXT1 与 Timer 唤醒配置、Light-sleep、唤醒来源锁存和
     * 临时配置清理。调用期间不会复位芯片；唤醒后继续执行原 Application 生命周期。
     *
     * @param[in] timer_wakeup_ms Timer 唤醒间隔，单位毫秒，必须大于 0
     * @param[out] out_wakeup 唤醒来源，仅在 ESP_OK 时有效
     * @return ESP_OK 已唤醒且清理完成；ESP_ERR_INVALID_ARG 参数无效；
     *         ESP_ERR_INVALID_STATE 按键尚未释放；或底层错误码
     */
    esp_err_t device_power_enter_light_sleep(uint32_t timer_wakeup_ms, device_power_wakeup_info_t *out_wakeup);

#ifdef __cplusplus
}
#endif
