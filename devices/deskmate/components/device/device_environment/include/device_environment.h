/**
 * @file device_environment.h
 * @brief 与传感器型号、I2C 和板级参数无关的同步环境能力
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 环境传感器状态快照 */
    typedef struct
    {
        int16_t  temperature_centi; /*!< 温度，单位 0.01°C */
        uint16_t humidity_centi;    /*!< 湿度，单位 0.01% */
        bool     valid;             /*!< 数据是否有效 */
    } device_environment_snapshot_t;

    /**
 * @brief 初始化同步环境传感器能力
 *
 * @return ESP_OK 初始化成功；其他值表示同步资源失败
 */
    esp_err_t device_environment_init(void);

    /**
 * @brief 在调用者上下文同步执行一次温湿度采样
 *
 * @return ESP_OK 采样和快照更新成功；其他值表示硬件读取失败
 */
    esp_err_t device_environment_sample(void);

    /**
 * @brief 获取最近一次环境传感器快照
 *
 * @param[out] out_snapshot 输出快照
 * @return ESP_OK 已输出；其他值表示参数或初始化状态错误
 */
    esp_err_t device_environment_get_snapshot_copy(device_environment_snapshot_t *out_snapshot);

    /**
 * @brief 释放环境快照锁和 BSP 传感器资源
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
 */
    esp_err_t device_environment_deinit(void);

#ifdef __cplusplus
}
#endif
