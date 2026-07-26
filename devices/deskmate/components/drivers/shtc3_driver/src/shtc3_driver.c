/*
 * 文件职责：实现 SHTC3 唤醒、测量、校验、换算和休眠协议。
 * 主要依赖：shtc3_driver.h、ESP-IDF I2C master driver、FreeRTOS。
 * 调用方：板级传感器适配层。
 */
#include "shtc3_driver.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shtc3_driver";

#define SHTC3_CMD_WAKEUP       0x3517
#define SHTC3_CMD_SLEEP        0xB098
#define SHTC3_CMD_MEASURE_T_RH 0x7866

static bool driver_is_valid(const shtc3_driver_t *driver)
{
    return driver != NULL && driver->i2c_device != NULL && driver->timeout_ms > 0;
}

static esp_err_t write_command(shtc3_driver_t *driver, uint16_t command)
{
    if (!driver_is_valid(driver))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t data[] = {
        (uint8_t) (command >> 8U),
        (uint8_t) (command & 0xFFU),
    };
    return i2c_master_transmit(driver->i2c_device, data, sizeof(data), driver->timeout_ms);
}

static uint8_t calculate_crc(const uint8_t data[2])
{
    uint8_t crc = 0xFFU;
    for (int i = 0; i < 2; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x80U) ? (uint8_t) ((crc << 1U) ^ 0x31U) : (uint8_t) (crc << 1U);
        }
    }
    return crc;
}

static int16_t temperature_centi_from_raw(uint16_t raw)
{
    const int32_t temperature = -4500 + (int32_t) (((int64_t) 17500 * raw + 32767) / 65535);
    return (int16_t) temperature;
}

static uint16_t humidity_centi_from_raw(uint16_t raw)
{
    return (uint16_t) (((uint32_t) 10000 * raw + 32767U) / 65535U);
}

esp_err_t shtc3_driver_init(shtc3_driver_t *driver, i2c_master_dev_handle_t i2c_device, uint32_t timeout_ms)
{
    if (driver == NULL || i2c_device == NULL || timeout_ms == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    driver->i2c_device = i2c_device;
    driver->timeout_ms = timeout_ms;
    esp_err_t err      = write_command(driver, SHTC3_CMD_WAKEUP);
    if (err == ESP_OK)
    {
        esp_rom_delay_us(500);
        err = write_command(driver, SHTC3_CMD_SLEEP);
    }
    if (err != ESP_OK)
    {
        driver->i2c_device = NULL;
        driver->timeout_ms = 0;
    }
    return err;
}

esp_err_t shtc3_driver_read_sample(shtc3_driver_t *driver, shtc3_sample_t *out)
{
    if (!driver_is_valid(driver) || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(write_command(driver, SHTC3_CMD_WAKEUP), TAG, "唤醒 SHTC3 失败");
    esp_rom_delay_us(500);

    esp_err_t err = write_command(driver, SHTC3_CMD_MEASURE_T_RH);
    if (err != ESP_OK)
    {
        (void) write_command(driver, SHTC3_CMD_SLEEP);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    uint8_t frame[6] = { 0 };
    err              = i2c_master_receive(driver->i2c_device, frame, sizeof(frame), driver->timeout_ms);
    (void) write_command(driver, SHTC3_CMD_SLEEP);
    if (err != ESP_OK)
    {
        return err;
    }
    if (calculate_crc(&frame[0]) != frame[2] || calculate_crc(&frame[3]) != frame[5])
    {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temperature = ((uint16_t) frame[0] << 8U) | frame[1];
    const uint16_t raw_humidity    = ((uint16_t) frame[3] << 8U) | frame[4];
    *out                           = (shtc3_sample_t) {
        .temperature_centi = temperature_centi_from_raw(raw_temperature),
        .humidity_centi    = humidity_centi_from_raw(raw_humidity),
    };
    return ESP_OK;
}

esp_err_t shtc3_driver_sleep(shtc3_driver_t *driver)
{
    return write_command(driver, SHTC3_CMD_SLEEP);
}
