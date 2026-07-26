/**
 * @file sht4x.c
 * @brief SHT4x 命令、CRC 校验与原始值换算
 */
#include "sht4x.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_rom_sys.h"

/** @brief 日志标签 */
static const char *TAG = "sht4x";

/** @brief SHT4x 软件复位命令 */
#define SHT4X_COMMAND_SOFT_RESET             0x94U
/** @brief SHT4x 读取序列号命令 */
#define SHT4X_COMMAND_READ_SERIAL            0x89U
/** @brief SHT4x 高精度测量命令 */
#define SHT4X_COMMAND_MEASURE_HIGH_PRECISION 0xFDU
/** @brief 单次 I2C 事务超时 */
#define SHT4X_I2C_TIMEOUT_MS                 100
/** @brief 软件复位和序列号命令后的等待时间 */
#define SHT4X_SHORT_WAIT_US                   1000U
/** @brief 高精度测量的最大等待时间，覆盖数据手册 8.3 ms */
#define SHT4X_MEASUREMENT_WAIT_US             9000U
/** @brief 瞬时通信故障后的重试间隔 */
#define SHT4X_MEASUREMENT_RETRY_WAIT_US        1000U
/** @brief 高精度测量的最大尝试次数 */
#define SHT4X_MEASUREMENT_MAX_ATTEMPTS         2U

/** @brief 计算 Sensirion 两字节数据的 CRC-8，初值 0xFF、多项式 0x31 */
static uint8_t sht4x_crc8(const uint8_t *data, size_t size_bytes)
{
    uint8_t crc = 0xFFU;
    for (size_t index = 0U; index < size_bytes; ++index)
    {
        crc ^= data[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc & 0x80U) != 0U ? (uint8_t) ((crc << 1U) ^ 0x31U)
                                      : (uint8_t) (crc << 1U);
        }
    }
    return crc;
}

/** @brief 校验 SHT4x 的两个数据字及其随附 CRC */
static esp_err_t sht4x_validate_response(const uint8_t response[6])
{
    if (sht4x_crc8(response, 2U) != response[2]
        || sht4x_crc8(&response[3], 2U) != response[5])
    {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

/** @brief 判断测量错误是否适合进行一次有界重试 */
static bool sht4x_measurement_error_is_retryable(esp_err_t error)
{
    return error == ESP_ERR_INVALID_RESPONSE || error == ESP_ERR_INVALID_CRC
           || error == ESP_ERR_TIMEOUT;
}

esp_err_t sht4x_init(sht4x_t *out_sensor, i2c_master_bus_handle_t bus,
                     uint16_t address_7bit, uint32_t scl_speed_hz)
{
    if (out_sensor == NULL || bus == NULL || address_7bit > 0x7FU || scl_speed_hz == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_sensor->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = i2c_master_probe(bus, address_7bit, SHT4X_I2C_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        return error;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address_7bit,
        .scl_speed_hz    = scl_speed_hz,
    };
    error = i2c_master_bus_add_device(bus, &device_config, &out_sensor->device);
    if (error != ESP_OK)
    {
        return error;
    }

    const uint8_t command = SHT4X_COMMAND_SOFT_RESET;
    error = i2c_master_transmit(out_sensor->device, &command, sizeof(command),
                                SHT4X_I2C_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        (void) i2c_master_bus_rm_device(out_sensor->device);
        out_sensor->device = NULL;
        return error;
    }

    esp_rom_delay_us(SHT4X_SHORT_WAIT_US);
    out_sensor->initialized = true;
    return ESP_OK;
}

esp_err_t sht4x_read_serial_number(const sht4x_t *sensor, uint32_t *out_serial_number)
{
    if (sensor == NULL || out_serial_number == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sensor->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t command = SHT4X_COMMAND_READ_SERIAL;
    esp_err_t error = i2c_master_transmit(sensor->device, &command, sizeof(command),
                                          SHT4X_I2C_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        return error;
    }
    esp_rom_delay_us(SHT4X_SHORT_WAIT_US);

    uint8_t response[6];
    error = i2c_master_receive(sensor->device, response, sizeof(response), SHT4X_I2C_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        return error;
    }
    error = sht4x_validate_response(response);
    if (error != ESP_OK)
    {
        return error;
    }

    *out_serial_number = ((uint32_t) response[0] << 24U) | ((uint32_t) response[1] << 16U)
                         | ((uint32_t) response[3] << 8U) | (uint32_t) response[4];
    return ESP_OK;
}

esp_err_t sht4x_measure_high_precision(const sht4x_t *sensor,
                                       sht4x_measurement_t *out_measurement)
{
    if (sensor == NULL || out_measurement == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sensor->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t attempt = 1U; attempt <= SHT4X_MEASUREMENT_MAX_ATTEMPTS; ++attempt)
    {
        const char   *failure_stage = "发送测量命令";
        const uint8_t command       = SHT4X_COMMAND_MEASURE_HIGH_PRECISION;
        esp_err_t     error         = i2c_master_transmit(sensor->device, &command,
                                                          sizeof(command),
                                                          SHT4X_I2C_TIMEOUT_MS);
        uint8_t response[6];
        if (error == ESP_OK)
        {
            esp_rom_delay_us(SHT4X_MEASUREMENT_WAIT_US);
            failure_stage = "读取测量响应";
            error = i2c_master_receive(sensor->device, response, sizeof(response),
                                       SHT4X_I2C_TIMEOUT_MS);
        }
        if (error == ESP_OK)
        {
            failure_stage = "校验测量响应";
            error         = sht4x_validate_response(response);
        }
        if (error == ESP_OK)
        {
            const uint16_t raw_temperature = ((uint16_t) response[0] << 8U) | response[1];
            const uint16_t raw_humidity    = ((uint16_t) response[3] << 8U) | response[4];
            out_measurement->temperature_c =
                -45.0F + (175.0F * (float) raw_temperature / 65535.0F);
            out_measurement->humidity_percent =
                -6.0F + (125.0F * (float) raw_humidity / 65535.0F);
            if (attempt > 1U)
            {
                ESP_LOGI(TAG, "SHT4x 高精度测量重试成功：尝试=%lu",
                         (unsigned long) attempt);
            }
            return ESP_OK;
        }

        const bool can_retry = attempt < SHT4X_MEASUREMENT_MAX_ATTEMPTS
                               && sht4x_measurement_error_is_retryable(error);
        if (!can_retry)
        {
            ESP_LOGE(TAG, "SHT4x 高精度测量失败：尝试=%lu，阶段=%s，错误=%s",
                     (unsigned long) attempt, failure_stage, esp_err_to_name(error));
            return error;
        }

        ESP_LOGW(TAG, "SHT4x 高精度测量出现瞬时故障，准备重试：尝试=%lu，阶段=%s，错误=%s",
                 (unsigned long) attempt, failure_stage, esp_err_to_name(error));
        esp_rom_delay_us(SHT4X_MEASUREMENT_RETRY_WAIT_US);
    }

    return ESP_FAIL;
}

esp_err_t sht4x_deinit(sht4x_t *sensor)
{
    if (sensor == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sensor->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = i2c_master_bus_rm_device(sensor->device);
    if (error != ESP_OK)
    {
        return error;
    }
    sensor->device      = NULL;
    sensor->initialized = false;
    return ESP_OK;
}
