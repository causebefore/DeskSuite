/**
 * @file bsp_led.c
 * @brief 实现 LED GPIO 的板级初始化与电平控制
 */
#include "bsp.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "board.h"

/** @brief 日志标签 */
static const char *TAG = "bsp_led";

/** @brief LED 模块是否已完成初始化 */
static bool s_initialized = false;

/** @brief LED 当前逻辑状态：true 表示点亮 */
static bool s_led_on = false;

/**
 * @brief 根据逻辑状态写入 GPIO 电平
 *
 * @param[in] on true 为点亮，false 为熄灭
 */
static void bsp_led_write_gpio(bool on)
{
    const uint32_t level = on ? (uint32_t)BOARD_LED_ACTIVE_HIGH
                              : (uint32_t)(1 - BOARD_LED_ACTIVE_HIGH);
    gpio_set_level(BOARD_LED_GPIO_NUM, (uint32_t)level);
}

esp_err_t bsp_led_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_LED_GPIO_NUM),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "LED GPIO 配置失败");

    /* 初始化后默认熄灭 */
    bsp_led_write_gpio(false);
    s_led_on      = false;
    s_initialized = true;

    ESP_LOGI(TAG, "LED 初始化完成 (GPIO%d, %s有效)",
             BOARD_LED_GPIO_NUM, BOARD_LED_ACTIVE_HIGH ? "高电平" : "低电平");
    return ESP_OK;
}

esp_err_t bsp_led_set_state(bool on)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bsp_led_write_gpio(on);
    s_led_on = on;
    return ESP_OK;
}

esp_err_t bsp_led_get_state(bool *out_on)
{
    if (out_on == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    *out_on = s_led_on;
    return ESP_OK;
}
