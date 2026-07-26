/*
 * 文件职责：定义屏幕相关板级参数。
 * 主要依赖：board_pins.h。
 * 调用方：bsp_display。
 */
#pragma once

#include "board_pins.h"

#include "driver/spi_common.h"

/** RLCD 屏幕水平分辨率，单位像素 */
#define BOARD_RLCD_WIDTH    400
/** RLCD 屏幕垂直分辨率，单位像素 */
#define BOARD_RLCD_HEIGHT   300
/** RLCD 屏幕使用的 SPI 主机编号 */
#define BOARD_RLCD_SPI_HOST SPI3_HOST
/** RLCD 屏幕 SPI 时钟频率，单位 Hz（40 MHz） */
#define BOARD_RLCD_SPI_HZ   (40 * 1000 * 1000)
