/* 文件职责：声明电池电压换算、滤波和低电判断纯逻辑。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

uint8_t  device_battery_percent_from_mv(uint16_t mv);
uint16_t device_battery_ema_mv(uint16_t previous_mv, uint16_t sample_mv, uint8_t alpha_percent);
bool     device_battery_is_low(uint16_t mv, uint16_t low_threshold_mv);
