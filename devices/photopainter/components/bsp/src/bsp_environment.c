/**
 * @file bsp_environment.c
 * @brief 在板载共享 I2C 总线上装配 SHT4x
 */
#include "bsp.h"

#include "board.h"
#include "bsp_i2c_internal.h"
#include "esp_log.h"
#include "sht4x.h"

/** @brief 日志标签 */
static const char *TAG = "bsp_environment";

/** @brief BSP 唯一持有的 SHT4x 驱动实例 */
static sht4x_t s_sensor;
/** @brief 初始化时读取并校验通过的传感器序列号 */
static uint32_t s_serial_number;
/** @brief 板载环境传感器生命周期状态 */
typedef enum
{
    BSP_ENVIRONMENT_STATE_UNINITIALIZED = 0,
    BSP_ENVIRONMENT_STATE_INITIALIZED,
    BSP_ENVIRONMENT_STATE_BUS_RELEASE_PENDING,
} bsp_environment_state_t;

/** @brief 当前板载环境传感器生命周期状态 */
static bsp_environment_state_t s_state;

esp_err_t bsp_environment_init(void)
{
    if (s_state != BSP_ENVIRONMENT_STATE_UNINITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_handle_t bus;
    esp_err_t error = bsp_i2c_acquire(&bus);
    if (error != ESP_OK)
    {
        return error;
    }

    error = sht4x_init(&s_sensor, bus, BOARD_SHT4X_ADDRESS, BOARD_I2C_SCL_SPEED_HZ);
    if (error == ESP_OK)
    {
        error = sht4x_read_serial_number(&s_sensor, &s_serial_number);
    }
    if (error != ESP_OK)
    {
        if (s_sensor.initialized)
        {
            (void) sht4x_deinit(&s_sensor);
        }
        (void) bsp_i2c_release();
        ESP_LOGE(TAG, "板载 SHT4x 初始化失败: %s", esp_err_to_name(error));
        return error;
    }

    s_state = BSP_ENVIRONMENT_STATE_INITIALIZED;
    ESP_LOGI(TAG, "板载 SHT4x 初始化完成，地址 0x%02X，序列号 %lu",
             BOARD_SHT4X_ADDRESS, (unsigned long) s_serial_number);
    return ESP_OK;
}

esp_err_t bsp_environment_get_serial_number(uint32_t *out_serial_number)
{
    if (out_serial_number == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != BSP_ENVIRONMENT_STATE_INITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    *out_serial_number = s_serial_number;
    return ESP_OK;
}

esp_err_t bsp_environment_measure(bsp_environment_measurement_t *out_measurement)
{
    if (out_measurement == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != BSP_ENVIRONMENT_STATE_INITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    sht4x_measurement_t measurement;
    const esp_err_t error = sht4x_measure_high_precision(&s_sensor, &measurement);
    if (error != ESP_OK)
    {
        return error;
    }
    out_measurement->temperature_c    = measurement.temperature_c;
    out_measurement->humidity_percent = measurement.humidity_percent;
    return ESP_OK;
}

esp_err_t bsp_environment_deinit(void)
{
    if (s_state == BSP_ENVIRONMENT_STATE_UNINITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == BSP_ENVIRONMENT_STATE_INITIALIZED)
    {
        const esp_err_t driver_error = sht4x_deinit(&s_sensor);
        if (driver_error != ESP_OK)
        {
            return driver_error;
        }
        s_state = BSP_ENVIRONMENT_STATE_BUS_RELEASE_PENDING;
    }

    const esp_err_t error = bsp_i2c_release();
    if (error != ESP_OK)
    {
        return error;
    }
    s_serial_number = 0U;
    s_state         = BSP_ENVIRONMENT_STATE_UNINITIALIZED;
    return ESP_OK;
}
