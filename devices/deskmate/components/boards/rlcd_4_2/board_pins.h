/*
 * 文件职责：集中定义板级 GPIO、总线引脚和未启用引脚占位值。
 * 主要依赖：无，仅提供硬件引脚宏。
 * 调用方：bsp_i2c、bsp_display、bsp_button、bsp_audio 等 BSP 模块。
 */
#pragma once

/** 未使用的引脚占位值，表示该功能未连接 */
#define BOARD_PIN_UNUSED    (-1)

/** RLCD 屏幕 DC（数据/命令选择）引脚 */
#define BOARD_RLCD_PIN_DC   5
/** RLCD 屏幕 CS（片选）引脚 */
#define BOARD_RLCD_PIN_CS   40
/** RLCD 屏幕 SPI 时钟引脚 */
#define BOARD_RLCD_PIN_SCLK 11
/** RLCD 屏幕 SPI MOSI（主出从入）引脚 */
#define BOARD_RLCD_PIN_MOSI 12
/** RLCD 屏幕复位引脚 */
#define BOARD_RLCD_PIN_RST  41
/** RLCD 屏幕 TE（撕裂效应同步）引脚 */
#define BOARD_RLCD_PIN_TE   6

/** 左按键 GPIO 编号 */
#define BOARD_PIN_BTN_LEFT  18
/** 右按键 GPIO 编号 */
#define BOARD_PIN_BTN_RIGHT 0

/** I2C 总线 SDA（数据线）引脚 */
#define BOARD_I2C_PIN_SDA   13
/** I2C 总线 SCL（时钟线）引脚 */
#define BOARD_I2C_PIN_SCL   14
/** I2C 总线端口号 */
#define BOARD_I2C_PORT      0
/** I2C 总线时钟频率，单位 Hz（400 kHz = Fast Mode） */
#define BOARD_I2C_FREQ_HZ   400000

/** PCF85063 RTC INT（闹钟/定时器中断）引脚 */
#define BOARD_RTC_PIN_INT   15
