/*
 * 文件职责：定义外部 RTC 相关的板级地址和寄存器布局常量。
 * 主要依赖：无。
 * 调用方：bsp_rtc。
 */
#pragma once

/** PCF85063A RTC 芯片 I2C 7-bit 地址 */
#define BOARD_RTC_PCF85063_ADDR 0x51
