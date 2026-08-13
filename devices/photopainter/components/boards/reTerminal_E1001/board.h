/**
 * @file board_pins.h
 * @brief 当前硬件目标的引脚与电气参数声明
 *
 * 本文件只声明静态硬件参数，不创建运行时资源或调用 Driver。
 * 修改引脚或电气特性时只需改动此处，上层组件无需变更。
 */
#pragma once

#include "hal/gpio_types.h"

/* ── 按键引脚定义 ─────────────────────────────────────────────────────── */

/** @brief 左按键 GPIO 引脚号 */
#define BOARD_BUTTON_LEFT_GPIO          (GPIO_NUM_5)
/** @brief 右按键 GPIO 引脚号 */
#define BOARD_BUTTON_RIGHT_GPIO         (GPIO_NUM_4)
/** @brief 确认按键 GPIO 引脚号 */
#define BOARD_BUTTON_CONFIRM_GPIO       (GPIO_NUM_3)

/**
 * @brief 各按键电气极性
 *
 * true  = 低电平有效（按下接地，需内部上拉）
 * false = 高电平有效（按下接 VCC）
 */
#define BOARD_BUTTON_LEFT_ACTIVE_LOW    true
#define BOARD_BUTTON_RIGHT_ACTIVE_LOW   true
#define BOARD_BUTTON_CONFIRM_ACTIVE_LOW true

/** @brief 三个按键的 GPIO 位掩码，供输入配置与睡眠唤醒源复用 */
#define BOARD_BUTTON_GPIO_MASK                                         \
    ((1ULL << BOARD_BUTTON_LEFT_GPIO) | (1ULL << BOARD_BUTTON_RIGHT_GPIO)    \
     | (1ULL << BOARD_BUTTON_CONFIRM_GPIO))

/**
 * @brief 用于 EXT1 睡眠唤醒的三个按键 GPIO 掩码
 *
 * 左、右、确认按键均为低电平有效的睡眠唤醒源。本常量只描述板级能力，实际唤醒源由
 * Device/BSP 电源链路在进入睡眠前配置。
 */
#define BOARD_BUTTON_WAKEUP_GPIO_MASK BOARD_BUTTON_GPIO_MASK

/* ── SD 卡引脚定义 ────────────────────────────────────────────────────── */

/** @brief SD 卡槽电源使能 GPIO，高电平开启供电 */
#define BOARD_SD_EN_GPIO              (GPIO_NUM_16)
/** @brief SD 卡插入检测 GPIO，低电平表示已插卡 */
#define BOARD_SD_DET_GPIO             (GPIO_NUM_15)
/** @brief SD 卡 SPI 片选 GPIO */
#define BOARD_SD_CS_GPIO              (GPIO_NUM_14)
/** @brief SD 卡 SPI MOSI GPIO，与墨水屏共享 */
#define BOARD_SD_MOSI_GPIO            (GPIO_NUM_9)
/** @brief SD 卡 SPI MISO GPIO */
#define BOARD_SD_MISO_GPIO            (GPIO_NUM_8)
/** @brief SD 卡 SPI 时钟 GPIO，与墨水屏共享 */
#define BOARD_SD_SCK_GPIO             (GPIO_NUM_7)
/** @brief SD 卡槽电源使能有效电平 */
#define BOARD_SD_EN_ACTIVE_HIGH       true
/** @brief SD 卡插入检测有效电平 */
#define BOARD_SD_DET_ACTIVE_LOW       true

/* ── LED 引脚定义 ─────────────────────────────────────────────────────── */

/** @brief LED 连接的 GPIO 编号 */
#define BOARD_LED_GPIO_NUM            (GPIO_NUM_6)

/** @brief LED 有效电平：1 为高电平点亮，0 为低电平点亮 */
#define BOARD_LED_ACTIVE_HIGH         (1)


/* ── 板载 I2C 与设备定义 ──────────────────────────────────────────────── */

/** @brief 板载外设共用的 I2C 控制器编号 */
#define BOARD_I2C_PORT_NUM            (0)
/** @brief 板载外设共用的 I2C SDA 引脚 */
#define BOARD_I2C_SDA_GPIO            (GPIO_NUM_19)
/** @brief 板载外设共用的 I2C SCL 引脚 */
#define BOARD_I2C_SCL_GPIO            (GPIO_NUM_20)
/** @brief 板载 I2C 设备通信速率 */
#define BOARD_I2C_SCL_SPEED_HZ        (400000U)
/** @brief SHT4x 的 7 位 I2C 地址 */
#define BOARD_SHT4X_ADDRESS           (0x44U)
/** @brief PCF8563 的 7 位 I2C 地址 */
#define BOARD_PCF8563_ADDRESS         (0x51U)


/* ── 电池电压监测引脚定义 ─────────────────────────────────────────────────────── */

/** @brief 电池分压采样 ADC GPIO */
#define BOARD_BATTERY_ADC_GPIO                   (GPIO_NUM_1)
/** @brief 电池监测电路使能 GPIO */
#define BOARD_BATTERY_MONITOR_ENABLE_GPIO        (GPIO_NUM_21)
/** @brief 电池监测电路使能有效电平 */
#define BOARD_BATTERY_MONITOR_ENABLE_ACTIVE_HIGH true
/** @brief 电池采样电路稳定等待时间，单位 ms */
#define BOARD_BATTERY_MONITOR_SETTLE_TIME_MS     (200U)
/** @brief 电池采样分压补偿倍率 */
#define BOARD_BATTERY_VOLTAGE_DIVIDER_RATIO      (2U)

/* ── 蜂鸣器引脚定义 ───────────────────────────────────────────────────── */

/** @brief 无源蜂鸣器 PWM 输出 GPIO */
#define BOARD_BUZZER_GPIO                        (GPIO_NUM_45)


/* ── MIC 定义 ─────────────────────────────────────────────────────── */
#define BOARD_PDM_CLK_GPIO            (GPIO_NUM_42)  // 输出到麦克风的时钟
#define BOARD_PDM_DATA_GPIO           (GPIO_NUM_41)  // 输出到麦克风的数据
#define BOARD_PDM_PWR_EN_GPIO         (GPIO_NUM_38)  // 输出到麦克风的电源使能

/* ── 墨水屏定义 ─────────────────────────────────────────────────────── */
#define BOARD_EPD_SCK_GPIO            GPIO_NUM_7
#define BOARD_EPD_MOSI_GPIO           GPIO_NUM_9
#define BOARD_EPD_CS_GPIO             GPIO_NUM_10
#define BOARD_EPD_DC_GPIO             GPIO_NUM_11
#define BOARD_EPD_RES_GPIO            GPIO_NUM_12
#define BOARD_EPD_BUSY_GPIO           GPIO_NUM_13

/** @brief 墨水屏 BUSY 低电平表示控制器正在工作 */
#define BOARD_EPD_BUSY_ACTIVE_LOW     true
