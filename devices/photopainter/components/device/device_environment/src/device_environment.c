/**
 * @file device_environment.c
 * @brief 把板载温湿度传感器收敛为稳定的 Device API
 */
#include "device_environment.h"

#include <stdbool.h>

#include "bsp.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "device_environment";

/** @brief Device 环境数据能力生命周期状态 */
static bool s_initialized;

esp_err_t device_environment_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = bsp_environment_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "BSP 环境传感器初始化失败: %s", esp_err_to_name(error));
        return error;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "设备环境数据能力初始化完成");
    return ESP_OK;
}

esp_err_t device_environment_get_serial_number(uint32_t *out_serial_number)
{
    if (out_serial_number == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_environment_get_serial_number(out_serial_number);
}

esp_err_t device_environment_measure(device_environment_measurement_t *out_measurement)
{
    if (out_measurement == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bsp_environment_measurement_t measurement;
    const esp_err_t error = bsp_environment_measure(&measurement);
    if (error != ESP_OK)
    {
        return error;
    }
    out_measurement->temperature_c    = measurement.temperature_c;
    out_measurement->humidity_percent = measurement.humidity_percent;
    return ESP_OK;
}

esp_err_t device_environment_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = bsp_environment_deinit();
    if (error != ESP_OK)
    {
        return error;
    }
    s_initialized = false;
    return ESP_OK;
}
