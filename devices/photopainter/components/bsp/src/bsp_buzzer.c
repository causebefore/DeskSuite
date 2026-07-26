/**
 * @file bsp_buzzer.c
 * @brief 实现无源蜂鸣器 LEDC PWM 输出
 */
#include "bsp.h"

#include "board.h"
#include "driver/ledc.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "bsp_buzzer";

/** @brief 蜂鸣器占用的 LEDC 资源 */
#define BSP_BUZZER_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BSP_BUZZER_LEDC_TIMER      LEDC_TIMER_0
#define BSP_BUZZER_LEDC_CHANNEL    LEDC_CHANNEL_0
#define BSP_BUZZER_DUTY_RESOLUTION LEDC_TIMER_10_BIT
#define BSP_BUZZER_DUTY_MAX        ((1U << 10U) - 1U)
#define BSP_BUZZER_INITIAL_FREQ_HZ (1000U)

/** @brief 蜂鸣器模块是否已完成初始化 */
static bool s_initialized;

esp_err_t bsp_buzzer_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode      = BSP_BUZZER_LEDC_MODE,
        .duty_resolution = BSP_BUZZER_DUTY_RESOLUTION,
        .timer_num       = BSP_BUZZER_LEDC_TIMER,
        .freq_hz         = BSP_BUZZER_INITIAL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t error = ledc_timer_config(&timer_config);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "配置蜂鸣器 LEDC 定时器失败: %s", esp_err_to_name(error));
        return error;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num   = BOARD_BUZZER_GPIO,
        .speed_mode = BSP_BUZZER_LEDC_MODE,
        .channel    = BSP_BUZZER_LEDC_CHANNEL,
        .timer_sel  = BSP_BUZZER_LEDC_TIMER,
        .duty       = 0U,
        .hpoint     = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    error = ledc_channel_config(&channel_config);
    if (error != ESP_OK)
    {
        (void) ledc_timer_pause(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_TIMER);
        const ledc_timer_config_t rollback_config = {
            .speed_mode  = BSP_BUZZER_LEDC_MODE,
            .timer_num   = BSP_BUZZER_LEDC_TIMER,
            .deconfigure = true,
        };
        (void) ledc_timer_config(&rollback_config);
        ESP_LOGE(TAG, "配置蜂鸣器 LEDC 通道失败: %s", esp_err_to_name(error));
        return error;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "蜂鸣器 PWM 初始化完成 (GPIO%d)", (int) BOARD_BUZZER_GPIO);
    return ESP_OK;
}

esp_err_t bsp_buzzer_start(uint32_t frequency_hz, uint8_t duty_percent)
{
    if (frequency_hz == 0U || duty_percent == 0U || duty_percent > 50U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = ledc_set_freq(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_TIMER, frequency_hz);
    if (error != ESP_OK)
    {
        return error;
    }
    const uint32_t duty =
        (BSP_BUZZER_DUTY_MAX * (uint32_t) duty_percent + 50U) / 100U;
    error = ledc_set_duty(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL, duty);
    if (error != ESP_OK)
    {
        return error;
    }
    return ledc_update_duty(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL);
}

esp_err_t bsp_buzzer_stop(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return ledc_stop(BSP_BUZZER_LEDC_MODE, BSP_BUZZER_LEDC_CHANNEL, 0U);
}
