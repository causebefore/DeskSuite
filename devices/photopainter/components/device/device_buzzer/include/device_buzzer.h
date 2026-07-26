/**
 * @file device_buzzer.h
 * @brief 与板型、GPIO 和 LEDC 细节无关的设备蜂鸣器能力
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化设备蜂鸣器能力，初始化后保持静音
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或 BSP 初始化错误码
     */
    esp_err_t device_buzzer_init(void);

    /**
     * @brief 以指定频率和占空比启动音调
     *
     * 调用方必须串行访问 device_buzzer_* API。
     *
     * @param[in] frequency_hz 音调频率，单位 Hz，必须大于 0
     * @param[in] duty_percent PWM 高电平占空比，范围 1～50；较小值通常声音更轻
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或 BSP LEDC 错误码
     */
    esp_err_t device_buzzer_start_tone(uint32_t frequency_hz, uint8_t duty_percent);

    /**
     * @brief 停止蜂鸣器音调并保持静音
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP LEDC 错误码
     */
    esp_err_t device_buzzer_stop(void);

#ifdef __cplusplus
}
#endif
