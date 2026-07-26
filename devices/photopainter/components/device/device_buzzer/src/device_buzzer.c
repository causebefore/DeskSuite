/**
 * @file device_buzzer.c
 * @brief 实现设备蜂鸣器同步音调播放能力
 */
#include "device_buzzer.h"

#include "bsp.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "device_buzzer";

/** @brief 蜂鸣器能力是否已初始化 */
static bool s_initialized;

esp_err_t device_buzzer_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = bsp_buzzer_init();
    if (error != ESP_OK)
    {
        return error;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "设备蜂鸣器能力初始化完成");
    return ESP_OK;
}

esp_err_t device_buzzer_start_tone(uint32_t frequency_hz, uint8_t duty_percent)
{
    if (frequency_hz == 0U || duty_percent == 0U || duty_percent > 50U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return bsp_buzzer_start(frequency_hz, duty_percent);
}

esp_err_t device_buzzer_stop(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_buzzer_stop();
}
