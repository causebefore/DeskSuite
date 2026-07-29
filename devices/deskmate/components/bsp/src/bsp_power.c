/**
 * @file bsp_power.c
 * @brief 按编译配置执行双按键与维护源轻睡眠唤醒
 */
#include "bsp.h"

#include <stddef.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "bsp_power";

#define BSP_POWER_BUTTON_WAKEUP_MASK ((1ULL << BOARD_PIN_BTN_LEFT) | (1ULL << BOARD_PIN_BTN_RIGHT))

#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
    #define BSP_POWER_WAKEUP_MASK (BSP_POWER_BUTTON_WAKEUP_MASK | (1ULL << BOARD_RTC_PIN_INT))
#else
    #define BSP_POWER_WAKEUP_MASK BSP_POWER_BUTTON_WAKEUP_MASK
#endif

static const bsp_button_id_t s_buttons[] = {
    BSP_BUTTON_LEFT,
    BSP_BUTTON_RIGHT,
};

_Static_assert(BOARD_PIN_BTN_LEFT >= 0 && BOARD_PIN_BTN_LEFT < GPIO_NUM_MAX, "左键 GPIO 必须是有效数字 IO");
_Static_assert(BOARD_PIN_BTN_RIGHT >= 0 && BOARD_PIN_BTN_RIGHT < GPIO_NUM_MAX, "右键 GPIO 必须是有效数字 IO");
_Static_assert(BOARD_PIN_BTN_LEFT != BOARD_PIN_BTN_RIGHT, "左右按键不能共用 GPIO");
#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
_Static_assert(BOARD_RTC_PIN_INT >= 0 && BOARD_RTC_PIN_INT < GPIO_NUM_MAX, "RTC INT GPIO 必须是有效数字 IO");
_Static_assert(BOARD_RTC_PIN_INT != BOARD_PIN_BTN_LEFT && BOARD_RTC_PIN_INT != BOARD_PIN_BTN_RIGHT,
               "RTC INT 不能与按键共用 GPIO");
#endif

/**
 * @brief 确认左右按键均处于释放高电平
 *
 * @return ESP_OK 两个按键均已释放；ESP_ERR_INVALID_STATE 任一按键仍按下；或按键读取错误码
 */
static esp_err_t ensure_wakeup_buttons_released(void)
{
    for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]); ++index)
    {
        bool            high  = false;
        const esp_err_t error = bsp_button_read_level(s_buttons[index], &high);
        if (error != ESP_OK)
        {
            return error;
        }
        if (!high)
        {
            ESP_LOGW(TAG,
                     "%s GPIO%d 尚未释放，拒绝进入轻睡眠",
                     s_buttons[index] == BSP_BUTTON_LEFT ? "左键" : "右键",
                     s_buttons[index] == BSP_BUTTON_LEFT ? BOARD_PIN_BTN_LEFT : BOARD_PIN_BTN_RIGHT);
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
/**
 * @brief 为无外部上拉的 RTC INT 保持睡眠域内部上拉
 *
 * 入睡前只改变 GPIO15 在 Light-sleep 边界的 RTC 域电气配置，不读取引脚电平。显式保持
 * RTC_PERIPH 供电可以判断此前的睡眠拒绝是否源于数字 GPIO 上拉在睡眠切换时失效；若 IDF
 * 仍拒绝睡眠，返回路径再单独执行电平与寄存器诊断。
 *
 * @return ESP_OK RTC 域上拉与电源策略已配置；其他值表示配置失败
 */
static esp_err_t configure_rtc_int_sleep_pullup(void)
{
    const gpio_num_t rtc_int_gpio = (gpio_num_t) BOARD_RTC_PIN_INT;
    esp_err_t        error        = rtc_gpio_pulldown_dis(rtc_int_gpio);
    if (error == ESP_OK)
    {
        error = rtc_gpio_pullup_en(rtc_int_gpio);
    }
    if (error == ESP_OK)
    {
        error = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "配置 RTC INT GPIO%d 睡眠域内部上拉失败: %s", BOARD_RTC_PIN_INT, esp_err_to_name(error));
        return error;
    }

    ESP_LOGI(TAG, "RTC INT GPIO%d 已启用 RTC 域内部上拉，Light-sleep 保持 RTC_PERIPH 供电", BOARD_RTC_PIN_INT);
    return ESP_OK;
}

/** @brief 恢复由本轮 RTC INT 测试临时覆盖的 RTC_PERIPH 自动电源策略 */
static esp_err_t restore_rtc_periph_auto_power_policy(void)
{
    const esp_err_t error = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "恢复 RTC_PERIPH 自动电源策略失败: %s", esp_err_to_name(error));
    }
    return error;
}

/** @brief 在 IDF 拒绝 Light-sleep 后单次诊断 RTC 是否持续主动拉低 INT */
static void diagnose_rtc_int_sleep_rejection(void)
{
    ESP_LOGW(TAG, "Light-sleep 被 IDF 拒绝，开始诊断 RTC INT GPIO%d", BOARD_RTC_PIN_INT);

    bool            asserted = false;
    const esp_err_t error    = bsp_rtc_read_interrupt_asserted(&asserted);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "诊断 RTC INT GPIO%d 失败: %s", BOARD_RTC_PIN_INT, esp_err_to_name(error));
        return;
    }
    if (!asserted)
    {
        ESP_LOGW(TAG,
                 "RTC INT 诊断完成: GPIO%d 当前已释放，睡眠拒绝可能来自短暂低脉冲或其他 EXT1 源",
                 BOARD_RTC_PIN_INT);
    }
}
#endif

esp_err_t bsp_power_enter_light_sleep(uint32_t timer_wakeup_ms, bsp_power_wakeup_result_t *out_result)
{
    if (timer_wakeup_ms == 0U || out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result                  = (bsp_power_wakeup_result_t) { 0 };

    const esp_err_t button_error = ensure_wakeup_buttons_released();
    if (button_error != ESP_OK)
    {
        return button_error;
    }

#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
    esp_err_t operation_error = configure_rtc_int_sleep_pullup();
    if (operation_error != ESP_OK)
    {
        return operation_error;
    }
#else
    esp_err_t operation_error = ESP_OK;
#endif

    operation_error = esp_sleep_enable_ext1_wakeup_io(BSP_POWER_WAKEUP_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    if (operation_error != ESP_OK)
    {
        ESP_LOGE(TAG, "配置 EXT1 轻睡眠唤醒失败: %s", esp_err_to_name(operation_error));
#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
        (void) restore_rtc_periph_auto_power_policy();
#endif
        return operation_error;
    }

#ifndef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
    operation_error = esp_sleep_enable_timer_wakeup((uint64_t) timer_wakeup_ms * 1000ULL);
    if (operation_error != ESP_OK)
    {
        ESP_LOGE(TAG, "配置内部 Timer 轻睡眠唤醒失败: %s", esp_err_to_name(operation_error));
        const esp_err_t cleanup_error = esp_sleep_disable_ext1_wakeup_io(BSP_POWER_WAKEUP_MASK);
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚按键 EXT1 轻睡眠唤醒配置失败: %s", esp_err_to_name(cleanup_error));
        }
        return operation_error;
    }
#endif

    /*
     * IDF v6.0.1 默认对 console UART 采用 SUSPEND 策略：入睡前对 UART0 执行 XOFF
     * 暂停，唤醒后再 XON 恢复。ESP32-S3 上实测唤醒后 UART0 日志静默，疑似 XON
     * 恢复未生效。改为 FLUSH：入睡前等待 TX FIFO 全部发完，不做 XOFF，从而规避
     * 唤醒后的 XON 恢复问题，同时保证入睡前的调试日志完整可见。
     */
    (void) esp_sleep_set_console_uart_handling_mode(ESP_SLEEP_ALWAYS_FLUSH_UART);

#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
    ESP_LOGI(TAG,
             "进入轻睡眠，左键 GPIO%d、右键 GPIO%d 或 RTC INT GPIO%d 可唤醒，"
             "RTC 域内部上拉已保持，内部 Timer 已禁用",
             BOARD_PIN_BTN_LEFT,
             BOARD_PIN_BTN_RIGHT,
             BOARD_RTC_PIN_INT);
#else
    ESP_LOGI(TAG,
             "进入轻睡眠，左键 GPIO%d、右键 GPIO%d 或内部 Timer %lu ms 可唤醒",
             BOARD_PIN_BTN_LEFT,
             BOARD_PIN_BTN_RIGHT,
             (unsigned long) timer_wakeup_ms);
#endif
    operation_error = esp_light_sleep_start();
    if (operation_error != ESP_OK)
    {
        ESP_LOGE(TAG, "进入轻睡眠失败: %s", esp_err_to_name(operation_error));
    }
    else
    {
        const uint32_t wakeup_causes      = esp_sleep_get_wakeup_causes();
        const uint64_t ext1_wakeup_status = esp_sleep_get_ext1_wakeup_status();
        *out_result                       = (bsp_power_wakeup_result_t) {
            .left_button  = (ext1_wakeup_status & (1ULL << BOARD_PIN_BTN_LEFT)) != 0U,
            .right_button = (ext1_wakeup_status & (1ULL << BOARD_PIN_BTN_RIGHT)) != 0U,
#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
            .rtc_alarm = (ext1_wakeup_status & (1ULL << BOARD_RTC_PIN_INT)) != 0U,
#endif
            .timer = (wakeup_causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0U,
        };
        ESP_LOGI(TAG,
                 "轻睡眠已结束，左键=%s，右键=%s，RTC INT=%s，Timer=%s，唤醒原因位图=0x%lx，"
                 "EXT1 状态=0x%llx",
                 out_result->left_button ? "是" : "否",
                 out_result->right_button ? "是" : "否",
                 out_result->rtc_alarm ? "是" : "否",
                 out_result->timer ? "是" : "否",
                 (unsigned long) wakeup_causes,
                 (unsigned long long) ext1_wakeup_status);
    }

    esp_err_t cleanup_error = esp_sleep_disable_ext1_wakeup_io(BSP_POWER_WAKEUP_MASK);
    if (cleanup_error != ESP_OK)
    {
        ESP_LOGE(TAG, "清理 EXT1 轻睡眠唤醒配置失败: %s", esp_err_to_name(cleanup_error));
    }
#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
    if (operation_error == ESP_ERR_SLEEP_REJECT)
    {
        diagnose_rtc_int_sleep_rejection();
    }
#endif
#ifndef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
    const esp_err_t timer_cleanup_error = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if (timer_cleanup_error != ESP_OK)
    {
        ESP_LOGE(TAG, "清理内部 Timer 轻睡眠唤醒配置失败: %s", esp_err_to_name(timer_cleanup_error));
        if (cleanup_error == ESP_OK)
        {
            cleanup_error = timer_cleanup_error;
        }
    }
#endif
#ifdef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
    const esp_err_t rtc_power_cleanup_error = restore_rtc_periph_auto_power_policy();
    if (rtc_power_cleanup_error != ESP_OK && cleanup_error == ESP_OK)
    {
        cleanup_error = rtc_power_cleanup_error;
    }
#endif
    return operation_error != ESP_OK ? operation_error : cleanup_error;
}
