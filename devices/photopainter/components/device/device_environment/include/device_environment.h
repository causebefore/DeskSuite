/**
 * @file device_environment.h
 * @brief 与传感器型号、总线和 GPIO 无关的设备环境数据能力
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 一次设备环境测量结果 */
    typedef struct
    {
        float temperature_c;
        float humidity_percent;
    } device_environment_measurement_t;

    /**
     * @brief 初始化设备环境数据能力
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或 BSP 错误码
     */
    esp_err_t device_environment_init(void);

    /**
     * @brief 获取当前环境传感器的硬件序列号
     *
     * @param[out] out_serial_number 32 位序列号，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t device_environment_get_serial_number(uint32_t *out_serial_number);

    /**
     * @brief 同步执行一次高精度环境测量
     *
     * 本函数同步阻塞约 9 ms。
     *
     * @param[out] out_measurement 温度和相对湿度，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或底层错误码
     */
    esp_err_t device_environment_measure(device_environment_measurement_t *out_measurement);

    /**
     * @brief 释放设备环境数据能力
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
     */
    esp_err_t device_environment_deinit(void);

#ifdef __cplusplus
}
#endif
