/**
 * @file device_led.c
 * @brief 实现设备级 LED 指示灯能力，委托 BSP 完成硬件操作
 */
#include "device_led.h"

#include "esp_log.h"
#include "esp_check.h"
#include "bsp.h"

/** @brief 日志标签 */
static const char *TAG    = "device_led";

/** @brief Device LED 模块是否已完成初始化 */
static bool s_initialized = false;

esp_err_t device_led_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_led_init(), TAG, "BSP LED 初始化失败");

    s_initialized = true;
    ESP_LOGI(TAG, "设备 LED 能力初始化完成");
    return ESP_OK;
}

esp_err_t device_led_on(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_led_set_state(true);
}

esp_err_t device_led_off(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_led_set_state(false);
}

esp_err_t device_led_toggle(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool      current = false;
    esp_err_t err     = bsp_led_get_state(&current);
    if (err != ESP_OK)
    {
        return err;
    }
    return bsp_led_set_state(!current);
}

esp_err_t device_led_is_on(bool *out_on)
{
    if (out_on == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_led_get_state(out_on);
}
