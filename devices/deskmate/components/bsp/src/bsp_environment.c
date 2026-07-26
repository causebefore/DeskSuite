/*
 * 文件职责：创建板级传感器 I2C 设备，组合 SHTC3 Driver 并应用校准。
 * 主要依赖：board_sensor.h、bsp_i2c、shtc3_driver。
 * 调用方：environment。
 */
#include "bsp.h"

#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "bsp_i2c_internal.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "shtc3_driver.h"

static const char *TAG = "bsp_environment";

#define BSP_SENSOR_I2C_TIMEOUT_MS 100

static bool                    s_ready;
static i2c_master_dev_handle_t s_i2c_device;
static shtc3_driver_t          s_driver;

esp_err_t bsp_environment_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "初始化 I2C 失败");
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_SHTC3_ADDR,
        .scl_speed_hz    = BOARD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_bus_handle(), &config, &s_i2c_device),
                        TAG,
                        "创建 SHTC3 I2C 设备失败");

    esp_err_t err = shtc3_driver_init(&s_driver, s_i2c_device, BSP_SENSOR_I2C_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        (void) i2c_master_bus_rm_device(s_i2c_device);
        s_i2c_device = NULL;
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "SHTC3 初始化完成: addr=0x%02x", BOARD_SHTC3_ADDR);
    return ESP_OK;
}

esp_err_t bsp_environment_read_sample(bsp_environment_sample_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    shtc3_sample_t sample;
    ESP_RETURN_ON_ERROR(shtc3_driver_read_sample(&s_driver, &sample), TAG, "读取 SHTC3 数据失败");

    const int32_t temperature = (int32_t) sample.temperature_centi + BOARD_SENSOR_TEMP_OFFSET_CENTI;
    int32_t       humidity    = (int32_t) sample.humidity_centi + BOARD_SENSOR_HUMI_OFFSET_CENTI;
    if (humidity < 0)
    {
        humidity = 0;
    }
    else if (humidity > 10000)
    {
        humidity = 10000;
    }

    out->temperature_centi = (int16_t) temperature;
    out->humidity_centi    = (uint16_t) humidity;
    return ESP_OK;
}

esp_err_t bsp_environment_deinit(void)
{
    if (!s_ready)
    {
        return ESP_OK;
    }

    esp_err_t result = shtc3_driver_sleep(&s_driver);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "SHTC3 进入 sleep 失败: %s", esp_err_to_name(result));
    }

    const esp_err_t remove_err = i2c_master_bus_rm_device(s_i2c_device);
    if (result == ESP_OK)
    {
        result = remove_err;
    }
    if (remove_err != ESP_OK)
    {
        ESP_LOGW(TAG, "移除 SHTC3 I2C 设备失败: %s", esp_err_to_name(remove_err));
    }

    s_i2c_device = NULL;
    memset(&s_driver, 0, sizeof(s_driver));
    s_ready = false;
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "SHTC3 反初始化完成（进入 sleep）");
    }
    return result;
}
