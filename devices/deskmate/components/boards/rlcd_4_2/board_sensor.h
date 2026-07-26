/*
 * 文件职责：定义温湿度传感器相关的板级地址和校准参数。
 * 主要依赖：无。
 * 调用方：bsp_environment。
 */
#pragma once

/** SHTC3 温湿度传感器 I2C 7-bit 地址 */
#define BOARD_SHTC3_ADDR               0x70
/** 温度偏移补偿，单位 0.01°C（正值上移、负值下移） */
#define BOARD_SENSOR_TEMP_OFFSET_CENTI 0
/** 湿度偏移补偿，单位 0.01%RH（正值上移、负值下移） */
#define BOARD_SENSOR_HUMI_OFFSET_CENTI 0
