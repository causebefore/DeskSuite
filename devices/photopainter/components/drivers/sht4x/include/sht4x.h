/**
 * @file sht4x.h
 * @brief SHT4x 温湿度传感器 I2C 驱动
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief SHT4x 驱动实例，由 BSP 提供静态存储 */
    typedef struct
    {
        i2c_master_dev_handle_t device;
        bool                    initialized;
    } sht4x_t;

    /** @brief SHT4x 一次温湿度测量结果 */
    typedef struct
    {
        float temperature_c;
        float humidity_percent;
    } sht4x_measurement_t;

    /**
     * @brief 在指定 I2C 总线上初始化 SHT4x 并执行软件复位
     *
     * @param[out] out_sensor 调用方提供且已清零的驱动实例
     * @param[in] bus 已由 BSP 创建并复位的 I2C master bus
     * @param[in] address_7bit SHT4x 的 7 位地址
     * @param[in] scl_speed_hz I2C 时钟频率
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NOT_FOUND 器件无响应；
     *         或底层 I2C 错误码
     */
    esp_err_t sht4x_init(sht4x_t *out_sensor, i2c_master_bus_handle_t bus,
                         uint16_t address_7bit, uint32_t scl_speed_hz);

    /**
     * @brief 读取并校验 SHT4x 唯一序列号
     *
     * 本函数同步阻塞约 1 ms。
     *
     * @param[in] sensor 已初始化的驱动实例
     * @param[out] out_serial_number 32 位序列号，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_CRC 响应 CRC 错误；或底层错误码
     */
    esp_err_t sht4x_read_serial_number(const sht4x_t *sensor, uint32_t *out_serial_number);

    /**
     * @brief 执行一次高精度温湿度测量并校验响应 CRC
     *
     * 本函数正常同步阻塞约 9 ms；遇到 NACK、CRC 或事务超时等瞬时故障时最多重试一次。
     * NACK/CRC 重试约 19 ms；若连续触发 100 ms I2C 超时，最坏约阻塞 220 ms。
     *
     * @param[in] sensor 已初始化的驱动实例
     * @param[out] out_measurement 测量结果，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_CRC 响应 CRC 错误；或底层错误码
     */
    esp_err_t sht4x_measure_high_precision(const sht4x_t *sensor,
                                           sht4x_measurement_t *out_measurement);

    /**
     * @brief 从 I2C 总线移除 SHT4x 并恢复未初始化状态
     *
     * @param[in,out] sensor 已初始化的驱动实例
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或底层错误码
     */
    esp_err_t sht4x_deinit(sht4x_t *sensor);

#ifdef __cplusplus
}
#endif
