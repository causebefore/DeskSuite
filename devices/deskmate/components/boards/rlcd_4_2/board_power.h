/*
 * 文件职责：定义电池采样相关的板级硬件参数。
 * 主要依赖：ESP-IDF ADC 类型定义。
 * 调用方：bsp_battery。
 */
#pragma once

#include "hal/adc_types.h"

/** 电池电压采样 ADC 引脚编号 */
#define BOARD_BATTERY_ADC_GPIO            4
/** 电池电压采样使用的 ADC 单元 */
#define BOARD_BATTERY_ADC_UNIT            ADC_UNIT_1
/** 电池电压采样使用的 ADC 通道 */
#define BOARD_BATTERY_ADC_CHANNEL         ADC_CHANNEL_3
/** 电池分压比分子（实际电压 = ADC 读值 × 分子 / 分母） */
#define BOARD_BATTERY_DIVIDER_NUMERATOR   3U
/** 电池分压比分母 */
#define BOARD_BATTERY_DIVIDER_DENOMINATOR 1U
