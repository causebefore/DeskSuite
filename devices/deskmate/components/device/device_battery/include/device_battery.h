/**
 * @file device_battery.h
 * @brief 与 ADC、GPIO 和板级分压参数无关的同步电池能力
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 电池状态快照 */
    typedef struct
    {
        uint16_t voltage_mv; /*!< 电池电压，单位毫伏 */
        uint8_t  percent;    /*!< 电量百分比 */
        bool     low;        /*!< 是否低电量 */
        bool     valid;      /*!< 数据是否有效 */
    } device_battery_snapshot_t;

    /**
 * @brief 初始化同步电池采样能力
 *
 * @return ESP_OK 初始化成功；其他值表示 BSP 或同步资源失败
 */
    esp_err_t device_battery_init(void);

    /**
 * @brief 在调用者上下文同步执行一次电池采样
 *
 * @return ESP_OK 采样和快照更新成功；其他值表示硬件读取失败
 */
    esp_err_t device_battery_sample(void);

    /**
 * @brief 获取最近一次电池快照
 *
 * @param[out] out_snapshot 输出快照
 * @return ESP_OK 已输出；其他值表示参数或初始化状态错误
 */
    esp_err_t device_battery_get_snapshot_copy(device_battery_snapshot_t *out_snapshot);

    /**
 * @brief 释放电池快照锁和 BSP ADC 资源
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
 */
    esp_err_t device_battery_deinit(void);

#ifdef __cplusplus
}
#endif
