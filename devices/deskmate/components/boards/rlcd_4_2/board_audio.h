/*
 * 文件职责：定义音频 Codec、I2S 引脚和总线地址等静态板级参数。
 * 主要依赖：board_pins.h。
 * 调用方：bsp_audio。
 */
#pragma once

#include "board_pins.h"

/** 音频 codec 使用的 I2C 总线端口（与其它外设共享同一条总线） */
#define BOARD_AUDIO_I2C_PORT         BOARD_I2C_PORT
/** ES8311 硬件 I2C 7-bit 地址 */
#define BOARD_AUDIO_ES8311_ADDR_7BIT 0x18
/** ES7210 硬件 I2C 7-bit 地址 */
#define BOARD_AUDIO_ES7210_ADDR_7BIT 0x40
/** 外部功放使能引脚（ES8311 DAC 输出经 PA 驱动扬声器） */
#define BOARD_AUDIO_PA_PIN           46
/** I2S MCLK（主时钟）引脚 */
#define BOARD_AUDIO_I2S_PIN_MCLK     16
/** I2S WS（字选择/帧同步）引脚 */
#define BOARD_AUDIO_I2S_PIN_WS       45
/** I2S BCLK（位时钟）引脚 */
#define BOARD_AUDIO_I2S_PIN_BCLK     9
/** I2S DOUT（数据输出，播放方向）引脚 */
#define BOARD_AUDIO_I2S_PIN_DOUT     8
/** I2S DIN（数据输入，录音方向）引脚 */
#define BOARD_AUDIO_I2S_PIN_DIN      10
