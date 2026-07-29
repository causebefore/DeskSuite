/**
 * @file bsp_rtc_internal.h
 * @brief BSP 内部 RTC 唤醒计时器事务
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 关闭 PCF85063 全部 INT 输出源并清除 AF/TF
 *
 * 该操作会关闭 CIE、AIE、MI、HMI、TE 和 TIE，并主动丢弃尚未消费的告警与计时器标志，
 * 仅用于 RTC INT 唤醒测试事务的释放基线。
 *
 * @return ESP_OK 已清理；ESP_ERR_INVALID_STATE RTC 未初始化；或 I2C 错误
 */
esp_err_t bsp_rtc_clear_interrupt_sources(void);

/**
 * @brief 启动独占 RTC INT 输出的秒级唤醒计时器
 *
 * 返回成功前已关闭并清除旧告警输出、清除旧 TF，并以 1 Hz 时钟启动 PCF85063 Timer。
 * 间隔向上取整到整秒，最大 255 秒。
 *
 * @param[in] interval_ms 唤醒间隔，单位毫秒，范围 1—255000
 * @return ESP_OK 已启动；ESP_ERR_INVALID_ARG 间隔无效；ESP_ERR_INVALID_STATE RTC 未初始化；
 *         或 I2C 错误
 */
esp_err_t bsp_rtc_start_wakeup_timer(uint32_t interval_ms);

/**
 * @brief 停止 RTC 唤醒计时器并清除计数值与 TF
 * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE RTC 未初始化；或 I2C 错误
 */
esp_err_t bsp_rtc_stop_wakeup_timer(void);
