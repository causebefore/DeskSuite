/**
 * @file bsp_buttons.c
 * @brief 装配板级按键 GPIO 与同步去抖状态机
 */
#include "bsp.h"

#include <stddef.h>
#include <stdatomic.h>

#include "board.h"
#include "button_driver.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "bsp_buttons";

/** @brief 单个板级按键的静态装配记录 */
typedef struct
{
    bsp_button_id_t id;
    gpio_num_t      gpio;
    bool            active_low;
    button_driver_t driver;
    atomic_bool     pressed_snapshot;
} bsp_button_slot_t;

/** @brief 三个物理按键及其通用状态机实例 */
static bsp_button_slot_t s_buttons[BSP_BUTTON_COUNT] = {
    [BSP_BUTTON_LEFT] = {
        .id         = BSP_BUTTON_LEFT,
        .gpio       = BOARD_BTN_LEFT_GPIO,
        .active_low = BOARD_BTN_LEFT_ACTIVE_LOW,
    },
    [BSP_BUTTON_RIGHT] = {
        .id         = BSP_BUTTON_RIGHT,
        .gpio       = BOARD_BTN_RIGHT_GPIO,
        .active_low = BOARD_BTN_RIGHT_ACTIVE_LOW,
    },
    [BSP_BUTTON_CONFIRM] = {
        .id         = BSP_BUTTON_CONFIRM,
        .gpio       = BOARD_BTN_CONFIRM_GPIO,
        .active_low = BOARD_BTN_CONFIRM_ACTIVE_LOW,
    },
};

/** @brief 上层借用的事件回调 */
static bsp_button_event_cb_t s_event_callback;

/** @brief 上层借用的事件上下文 */
static void *s_event_context;

/** @brief BSP 按键生命周期状态 */
static bool s_initialized;

/** @brief 读取一个按键 GPIO 原始电平 */
static int bsp_buttons_read_level(void *context)
{
    const bsp_button_slot_t *slot = context;
    return gpio_get_level(slot->gpio);
}

/** @brief 把 Driver 事件转换为 BSP 事件并同步通知 Device */
static void bsp_buttons_on_driver_event(const button_driver_t *button, button_driver_event_t event,
                                        uint8_t click_count, void *context)
{
    bsp_button_slot_t *slot = context;
    bool               pressed;
    if (button_driver_is_pressed(button, &pressed) == ESP_OK)
    {
        atomic_store_explicit(&slot->pressed_snapshot, pressed, memory_order_release);
    }
    if (s_event_callback != NULL)
    {
        s_event_callback(slot->id, (bsp_button_event_t) event, click_count, s_event_context);
    }
}

esp_err_t bsp_buttons_scan(uint32_t elapsed_ms)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (elapsed_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t index = 0; index < BSP_BUTTON_COUNT; ++index)
    {
        esp_err_t error = button_driver_tick(&s_buttons[index].driver, elapsed_ms);
        bool      pressed;
        if (error == ESP_OK)
        {
            error = button_driver_is_pressed(&s_buttons[index].driver, &pressed);
        }
        if (error == ESP_OK)
        {
            atomic_store_explicit(&s_buttons[index].pressed_snapshot,
                                  pressed,
                                  memory_order_release);
        }
        if (error != ESP_OK)
        {
            return error;
        }
    }
    return ESP_OK;
}

/** @brief 回滚已经初始化的 Driver 实例 */
static void bsp_buttons_deinit_drivers(size_t initialized_count)
{
    while (initialized_count > 0U)
    {
        --initialized_count;
        (void) button_driver_deinit(&s_buttons[initialized_count].driver);
    }
}

esp_err_t bsp_buttons_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = BOARD_BUTTON_GPIO_MASK,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_cfg), TAG, "按键 GPIO 配置失败");

    size_t initialized_count = 0U;
    for (; initialized_count < BSP_BUTTON_COUNT; ++initialized_count)
    {
        bool                   current_initialized = false;
        button_driver_config_t config;
        esp_err_t              error = button_driver_get_default_config(&config);
        if (error == ESP_OK)
        {
            config.active_level = s_buttons[initialized_count].active_low ? 0U : 1U;
            config.read_level   = bsp_buttons_read_level;
            config.read_context = &s_buttons[initialized_count];
            error               = button_driver_init(&s_buttons[initialized_count].driver, &config);
            current_initialized = error == ESP_OK;
        }
        if (error == ESP_OK)
        {
            error = button_driver_set_event_callback_borrow(&s_buttons[initialized_count].driver,
                                                            bsp_buttons_on_driver_event,
                                                            &s_buttons[initialized_count]);
        }
        if (error != ESP_OK)
        {
            if (current_initialized)
            {
                (void) button_driver_deinit(&s_buttons[initialized_count].driver);
            }
            bsp_buttons_deinit_drivers(initialized_count);
            for (size_t gpio_index = 0; gpio_index < BSP_BUTTON_COUNT; ++gpio_index)
            {
                (void) gpio_reset_pin(s_buttons[gpio_index].gpio);
            }
            ESP_LOGE(TAG,
                     "按键 %u 状态机初始化失败: %s",
                     (unsigned) initialized_count,
                     esp_err_to_name(error));
            return error;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "三个按键 GPIO 与同步状态机初始化完成");
    return ESP_OK;
}

esp_err_t bsp_buttons_set_event_callback_borrow(bsp_button_event_cb_t callback, void *context)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_event_callback = callback;
    s_event_context  = callback != NULL ? context : NULL;
    return ESP_OK;
}

esp_err_t bsp_buttons_is_pressed(bsp_button_id_t button, bool *out_pressed)
{
    if (button < BSP_BUTTON_LEFT || button >= BSP_BUTTON_COUNT || out_pressed == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    *out_pressed = atomic_load_explicit(&s_buttons[button].pressed_snapshot, memory_order_acquire);
    return ESP_OK;
}

esp_err_t bsp_buttons_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t first_error = ESP_OK;
    for (size_t index = 0; index < BSP_BUTTON_COUNT; ++index)
    {
        esp_err_t error = button_driver_deinit(&s_buttons[index].driver);
        if (first_error == ESP_OK && error != ESP_OK)
        {
            first_error = error;
        }
        error = gpio_reset_pin(s_buttons[index].gpio);
        if (first_error == ESP_OK && error != ESP_OK)
        {
            first_error = error;
        }
        atomic_store_explicit(&s_buttons[index].pressed_snapshot, false, memory_order_release);
    }

    s_event_callback = NULL;
    s_event_context  = NULL;
    s_initialized    = false;
    if (first_error != ESP_OK)
    {
        ESP_LOGE(TAG, "按键资源释放失败: %s", esp_err_to_name(first_error));
    }
    return first_error;
}
