/**
 * @file shtc3_driver.h
 * @brief SHTC3 芯片协议，不包含板级地址和校准参数
 */
#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        i2c_master_dev_handle_t i2c_device; /**< 由 BSP 创建并长期借用的 I2C Device */
        uint32_t                timeout_ms; /**< 单次命令事务超时，单位毫秒 */
    } shtc3_driver_t;

    /** @brief 已换算为统一定点单位的 SHTC3 样本 */
    typedef struct
    {
        int16_t  temperature_centi;
        uint16_t humidity_centi;
    } shtc3_sample_t;

    /**
     * @brief 初始化不拥有底层句柄的 SHTC3 Driver 实例
     * @param[out] driver Driver 实例
     * @param[in] i2c_device BSP 创建的 I2C Device，必须覆盖 Driver 使用期
     * @param[in] timeout_ms 单次 I2C 事务超时，单位毫秒
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效
     */
    esp_err_t shtc3_driver_init(shtc3_driver_t *driver, i2c_master_dev_handle_t i2c_device, uint32_t timeout_ms);

    /**
     * @brief 唤醒芯片、读取带 CRC 的温湿度样本并换算定点单位
     * @param[in] driver Driver 实例
     * @param[out] out 样本，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_CRC 校验失败；或 I2C 错误
     */
    esp_err_t shtc3_driver_read_sample(shtc3_driver_t *driver, shtc3_sample_t *out);

    /**
     * @brief 请求 SHTC3 进入睡眠
     * @param[in] driver Driver 实例
     * @return ESP_OK 成功；或 I2C 错误
     */
    esp_err_t shtc3_driver_sleep(shtc3_driver_t *driver);

#ifdef __cplusplus
}
#endif
