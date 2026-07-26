/**
 * @file device_led.h
 * @brief 设备级 LED 指示灯能力接口
 *
 * 对上层（Application / Service）提供设备语义的 LED 控制。
 * 内部委托 BSP 完成硬件操作，上层不得直接调用 BSP 或 Board 头文件。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化设备 LED 能力
 *
 * 内部调用 bsp_led_init() 完成 GPIO 硬件初始化。
 * 可重复调用，已初始化时直接返回 ESP_OK。
 *
 * @return ESP_OK 成功；或其他底层错误码
 */
esp_err_t device_led_init(void);

/**
 * @brief 点亮设备 LED
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t device_led_on(void);

/**
 * @brief 熄灭设备 LED
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t device_led_off(void);

/**
 * @brief 切换设备 LED 状态（亮→灭，灭→亮）
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t device_led_toggle(void);

/**
 * @brief 获取设备 LED 当前是否点亮
 *
 * @param[out] out_on 当前状态，true 为点亮
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t device_led_is_on(bool *out_on);

#ifdef __cplusplus
}
#endif
