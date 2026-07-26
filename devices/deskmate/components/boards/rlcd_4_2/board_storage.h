/*
 * 文件职责：定义外部 SD 卡的 1-bit SDMMC 卡槽、引脚和总线频率。
 * 主要依赖：ESP-IDF SDMMC Host 类型。
 * 调用方：bsp_storage。
 */
#pragma once

#include "driver/sdmmc_host.h"

/** SD 卡使用的 SDMMC Host 卡槽 */
#define BOARD_STORAGE_SDMMC_SLOT   SDMMC_HOST_SLOT_1
/** SD 卡 SDMMC 时钟引脚 */
#define BOARD_STORAGE_PIN_CLK      38
/** SD 卡 SDMMC 命令引脚 */
#define BOARD_STORAGE_PIN_CMD      21
/** SD 卡 SDMMC 数据线 D0 引脚 */
#define BOARD_STORAGE_PIN_D0       39
/** SD 卡 SDMMC 总线宽度 */
#define BOARD_STORAGE_BUS_WIDTH    1
/** SD 卡 SDMMC 最高时钟频率，单位 kHz */
#define BOARD_STORAGE_MAX_FREQ_KHZ 20000
