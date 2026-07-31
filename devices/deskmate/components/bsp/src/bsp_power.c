/**
 * @file bsp_power.c
 * @brief 执行双按键 EXT1 与内部 Timer 轻睡眠唤醒
 */
#include "bsp.h"

#include <stddef.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"

static const char *TAG = "bsp_power";

/** @brief 标记 console UART 处理模式是否已在初始化时配置 */
static bool s_uart_handling_initialized = false;

#define BSP_POWER_WAKEUP_MASK        ((1ULL << BOARD_PIN_BTN_LEFT) | (1ULL << BOARD_PIN_BTN_RIGHT))

static const bsp_button_id_t s_buttons[] = {
    BSP_BUTTON_LEFT,
    BSP_BUTTON_RIGHT,
};

_Static_assert(BOARD_PIN_BTN_LEFT >= 0 && BOARD_PIN_BTN_LEFT < GPIO_NUM_MAX, "左键 GPIO 必须是有效数字 IO");
_Static_assert(BOARD_PIN_BTN_RIGHT >= 0 && BOARD_PIN_BTN_RIGHT < GPIO_NUM_MAX, "右键 GPIO 必须是有效数字 IO");
_Static_assert(BOARD_PIN_BTN_LEFT != BOARD_PIN_BTN_RIGHT, "左右按键不能共用 GPIO");

/**
 * @brief 初始化轻睡眠相关的全局睡眠配置
 *
 * 在系统启动早期一次性设置 console UART 处理模式，避免每次入睡前重复调用。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 重复初始化；或底层错误码
 */
esp_err_t bsp_power_init(void)
{
    if (s_uart_handling_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * IDF v6.0.1 默认对 console UART 采用 SUSPEND 策略：入睡前对 UART0 执行 XOFF
     * 暂停，唤醒后再 XON 恢复。ESP32-S3 上实测唤醒后 UART0 日志静默，疑似 XON
     * 恢复未生效。改为 FLUSH：入睡前等待 TX FIFO 全部发完，不做 XOFF，从而规避
     * 唤醒后的 XON 恢复问题，同时保证入睡前的调试日志完整可见。
     */
    const esp_err_t error = esp_sleep_set_console_uart_handling_mode(ESP_SLEEP_ALWAYS_FLUSH_UART);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "配置 console UART 处理模式失败: %s", esp_err_to_name(error));
        return error;
    }

    s_uart_handling_initialized = true;
    ESP_LOGD(TAG, "轻睡眠 UART 处理模式已配置为 FLUSH");
    return ESP_OK;
}

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

    esp_err_t operation_error = esp_sleep_enable_ext1_wakeup_io(BSP_POWER_WAKEUP_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    if (operation_error != ESP_OK)
    {
        ESP_LOGE(TAG, "配置 EXT1 轻睡眠唤醒失败: %s", esp_err_to_name(operation_error));
        return operation_error;
    }

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

    ESP_LOGD(TAG,
             "进入轻睡眠，左键 GPIO%d、右键 GPIO%d 或内部 Timer %lu ms 可唤醒",
             BOARD_PIN_BTN_LEFT,
             BOARD_PIN_BTN_RIGHT,
             (unsigned long) timer_wakeup_ms);
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
            .timer        = (wakeup_causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0U,
        };
        ESP_LOGD(TAG,
                 "轻睡眠已结束，左键=%s，右键=%s，Timer=%s，唤醒原因位图=0x%lx，"
                 "EXT1 状态=0x%llx",
                 out_result->left_button ? "是" : "否",
                 out_result->right_button ? "是" : "否",
                 out_result->timer ? "是" : "否",
                 (unsigned long) wakeup_causes,
                 (unsigned long long) ext1_wakeup_status);
    }

    esp_err_t cleanup_error = esp_sleep_disable_ext1_wakeup_io(BSP_POWER_WAKEUP_MASK);
    if (cleanup_error != ESP_OK)
    {
        ESP_LOGE(TAG, "清理 EXT1 轻睡眠唤醒配置失败: %s", esp_err_to_name(cleanup_error));
    }
    const esp_err_t timer_cleanup_error = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    if (timer_cleanup_error != ESP_OK)
    {
        ESP_LOGE(TAG, "清理内部 Timer 轻睡眠唤醒配置失败: %s", esp_err_to_name(timer_cleanup_error));
        if (cleanup_error == ESP_OK)
        {
            cleanup_error = timer_cleanup_error;
        }
    }
    return operation_error != ESP_OK ? operation_error : cleanup_error;
}
