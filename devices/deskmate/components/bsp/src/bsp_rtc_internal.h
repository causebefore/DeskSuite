/**
 * @file bsp_rtc_internal.h
 * @brief BSP 内部 RTC 辅助接口
 *
 * 当前仅保留告警相关内部能力；RTC Timer 唤醒测试模式已移除，不再通过 PCF85063 Timer
 * 驱动 Light-sleep 维护唤醒。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
