/**
 * @file bsp_power.c
 * @brief 配置当前板型的按键、内部定时器深睡唤醒与整机深睡入口
 */
#include "bsp.h"

#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"

/** @brief 日志标签 */
static const char *TAG = "bsp_power";

/** @brief 按键 RTC 路由是否已为本次深睡准备完成 */
static bool s_deep_sleep_prepared;
/** @brief 本次准备的内部定时唤醒间隔，0 表示未启用 */
static uint64_t s_timer_wakeup_us;

/** @brief 当前板型允许从深睡唤醒设备的按键 GPIO */
static const gpio_num_t s_button_wakeup_gpios[] = {
    BOARD_BTN_LEFT_GPIO,
    BOARD_BTN_RIGHT_GPIO,
    BOARD_BTN_CONFIRM_GPIO,
};

_Static_assert(BOARD_BTN_LEFT_ACTIVE_LOW && BOARD_BTN_RIGHT_ACTIVE_LOW
                   && BOARD_BTN_CONFIRM_ACTIVE_LOW,
               "深睡按键唤醒要求三个按键均为低电平有效");

/**
 * @brief 释放全部按键的 RTC IO 路由并保留首个错误
 *
 * @return ESP_OK 全部释放；或首个底层错误码
 */
static esp_err_t bsp_power_deinit_button_wakeup_gpios(void)
{
    esp_err_t first_error = ESP_OK;
    for (size_t index = 0U;
         index < sizeof(s_button_wakeup_gpios) / sizeof(s_button_wakeup_gpios[0]);
         ++index)
    {
        const esp_err_t error = rtc_gpio_deinit(s_button_wakeup_gpios[index]);
        if (first_error == ESP_OK && error != ESP_OK)
        {
            first_error = error;
        }
    }
    return first_error;
}

esp_err_t bsp_power_prepare_deep_sleep(uint64_t timer_wakeup_us)
{
    if (s_deep_sleep_prepared)
    {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t index = 0U;
         index < sizeof(s_button_wakeup_gpios) / sizeof(s_button_wakeup_gpios[0]);
         ++index)
    {
        if (!esp_sleep_is_valid_wakeup_gpio(s_button_wakeup_gpios[index]))
        {
            ESP_LOGE(TAG,
                     "按键 GPIO%d 不支持深睡唤醒",
                     (int) s_button_wakeup_gpios[index]);
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t error = ESP_OK;
    for (size_t index = 0U;
         error == ESP_OK
         && index < sizeof(s_button_wakeup_gpios) / sizeof(s_button_wakeup_gpios[0]);
         ++index)
    {
        const gpio_num_t gpio = s_button_wakeup_gpios[index];
        error = rtc_gpio_init(gpio);
        if (error == ESP_OK)
        {
            error = rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_INPUT_ONLY);
        }
        if (error == ESP_OK)
        {
            error = rtc_gpio_pullup_en(gpio);
        }
        if (error == ESP_OK)
        {
            error = rtc_gpio_pulldown_dis(gpio);
        }
        if (error == ESP_OK && rtc_gpio_get_level(gpio) == 0U)
        {
            error = ESP_ERR_INVALID_STATE;
            ESP_LOGW(TAG, "按键 GPIO%d 尚未释放，拒绝进入深睡", (int) gpio);
        }
        else if (error != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "配置按键 GPIO%d 的 RTC 深睡输入失败: %s",
                     (int) gpio,
                     esp_err_to_name(error));
        }
    }
    if (error == ESP_OK)
    {
        error = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    }
    if (error == ESP_OK)
    {
        error = esp_sleep_enable_ext1_wakeup_io(BOARD_BUTTON_WAKEUP_GPIO_MASK,
                                                ESP_EXT1_WAKEUP_ANY_LOW);
    }
    if (error == ESP_OK && timer_wakeup_us > 0U)
    {
        error = esp_sleep_enable_timer_wakeup(timer_wakeup_us);
    }
    if (error != ESP_OK)
    {
        (void) esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        (void) bsp_power_deinit_button_wakeup_gpios();
        return error;
    }

    s_deep_sleep_prepared = true;
    s_timer_wakeup_us = timer_wakeup_us;
    return ESP_OK;
}

esp_err_t bsp_power_cancel_deep_sleep(void)
{
    esp_err_t error = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    const esp_err_t gpio_error = bsp_power_deinit_button_wakeup_gpios();
    s_deep_sleep_prepared = false;
    s_timer_wakeup_us = 0U;
    return error != ESP_OK ? error : gpio_error;
}

void bsp_power_start_deep_sleep(void)
{
    if (!s_deep_sleep_prepared)
    {
        ESP_LOGE(TAG, "深睡唤醒源尚未准备，设备将重启以避免无唤醒源休眠");
        esp_restart();
    }
    if (s_timer_wakeup_us > 0U)
    {
        ESP_LOGI(TAG,
                 "进入深睡，左/右/确认按键 GPIO%d/%d/%d 或内部定时器 %llu 秒可唤醒",
                 (int) BOARD_BTN_LEFT_GPIO,
                 (int) BOARD_BTN_RIGHT_GPIO,
                 (int) BOARD_BTN_CONFIRM_GPIO,
                 (unsigned long long) (s_timer_wakeup_us / 1000000ULL));
    }
    else
    {
        ESP_LOGI(TAG,
                 "进入深睡，仅左/右/确认按键 GPIO%d/%d/%d 可唤醒",
                 (int) BOARD_BTN_LEFT_GPIO,
                 (int) BOARD_BTN_RIGHT_GPIO,
                 (int) BOARD_BTN_CONFIRM_GPIO);
    }
    esp_deep_sleep_start();
}

esp_err_t bsp_power_was_woken_by_button(bool *out_woken_by_button)
{
    if (out_woken_by_button == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    *out_woken_by_button = (causes & (1UL << ESP_SLEEP_WAKEUP_EXT1)) != 0U
                           && (esp_sleep_get_ext1_wakeup_status()
                               & BOARD_BUTTON_WAKEUP_GPIO_MASK)
                                  != 0U;
    return ESP_OK;
}

esp_err_t bsp_power_was_woken_by_timer(bool *out_woken_by_timer)
{
    if (out_woken_by_timer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    *out_woken_by_timer = (causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0U;
    return ESP_OK;
}
