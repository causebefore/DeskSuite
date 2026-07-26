/**
 * @file button_driver.c
 * @brief 实现与平台无关的按键事件状态机
 */
#include "button_driver.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

/** @brief 判断原始电平是否处于按下态 */
static bool button_driver_level_is_active(const button_driver_t *button)
{
    return button->current_level == button->config.active_level;
}

/** @brief 将计时器按实际调度间隔饱和递增，避免长时间运行后回绕 */
static void button_driver_advance_elapsed(button_driver_t *button, uint32_t elapsed_ms)
{
    if (UINT32_MAX - button->elapsed_ms < elapsed_ms)
    {
        button->elapsed_ms = UINT32_MAX;
        return;
    }
    button->elapsed_ms += elapsed_ms;
}

/**
 * @brief 在过滤后同步发布一个不可变事件
 *
 * @param[in] button 事件来源
 * @param[in] event 事件类型
 */
static void button_driver_emit(button_driver_t *button, button_driver_event_t event)
{
    if ((button->config.event_mask & (1UL << (uint32_t) event)) == 0U
        || button->event_callback == NULL)
    {
        return;
    }

    const uint8_t click_count = button->click_count;
    button->event_callback(button, event, click_count, button->event_context);
}

/** @brief 检查所有时间、电平和回调配置是否可安全运行 */
static bool button_driver_config_is_valid(const button_driver_config_t *config)
{
    return config != NULL && config->read_level != NULL && config->debounce_ms > 0U
           && config->long_press_ms > 0U && config->long_press_hold_ms > 0U
           && config->multi_click_window_ms > 0U && config->active_level <= 1U
           && config->max_click_count > 0U;
}

esp_err_t button_driver_get_default_config(button_driver_config_t *out_config)
{
    if (out_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_config = (button_driver_config_t){
        .debounce_ms           = 20U,
        .long_press_ms         = 1000U,
        .long_press_hold_ms    = 100U,
        .multi_click_window_ms = 300U,
        .event_mask            = BUTTON_DRIVER_EVENT_MASK_ALL,
        .active_level          = 0U,
        .max_click_count       = 10U,
        .read_level            = NULL,
        .read_context          = NULL,
    };
    return ESP_OK;
}

esp_err_t button_driver_init(button_driver_t *out_button, const button_driver_config_t *config)
{
    if (out_button == NULL || !button_driver_config_is_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_button->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out_button, 0, sizeof(*out_button));
    out_button->config        = *config;
    out_button->state         = BUTTON_DRIVER_STATE_IDLE;
    out_button->current_level = config->read_level(config->read_context) != 0 ? 1U : 0U;
    out_button->startup_skip  = button_driver_level_is_active(out_button);
    out_button->initialized   = true;
    return ESP_OK;
}

esp_err_t button_driver_set_event_callback_borrow(button_driver_t         *button,
                                                  button_driver_event_cb_t callback, void *context)
{
    if (button == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!button->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    button->event_callback = callback;
    button->event_context  = callback != NULL ? context : NULL;
    return ESP_OK;
}

esp_err_t button_driver_tick(button_driver_t *button, uint32_t elapsed_ms)
{
    if (button == NULL || elapsed_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!button->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    button->current_level = button->config.read_level(button->config.read_context) != 0 ? 1U : 0U;

    if (button->startup_skip)
    {
        button->startup_skip = button_driver_level_is_active(button);
        return ESP_OK;
    }

    switch (button->state)
    {
        case BUTTON_DRIVER_STATE_IDLE:
            if (button_driver_level_is_active(button))
            {
                button->elapsed_ms = 0U;
                button->state      = BUTTON_DRIVER_STATE_DEBOUNCE_PRESS;
            }
            break;

        case BUTTON_DRIVER_STATE_DEBOUNCE_PRESS:
            if (!button_driver_level_is_active(button))
            {
                button->elapsed_ms = 0U;
                button->state = button->click_count == 0U ? BUTTON_DRIVER_STATE_IDLE
                                                          : BUTTON_DRIVER_STATE_WAIT_NEXT_CLICK;
                break;
            }

            button_driver_advance_elapsed(button, elapsed_ms);
            if (button->elapsed_ms >= button->config.debounce_ms)
            {
                button->pressed    = true;
                button->elapsed_ms = 0U;
                button->state      = BUTTON_DRIVER_STATE_PRESSED;
                button_driver_emit(button, BUTTON_DRIVER_EVENT_PRESS);
            }
            break;

        case BUTTON_DRIVER_STATE_DEBOUNCE_RELEASE:
            if (button_driver_level_is_active(button))
            {
                button->elapsed_ms = 0U;
                button->state = button->release_from_long_press ? BUTTON_DRIVER_STATE_LONG_PRESS
                                                                : BUTTON_DRIVER_STATE_PRESSED;
                break;
            }

            button_driver_advance_elapsed(button, elapsed_ms);
            if (button->elapsed_ms >= button->config.debounce_ms)
            {
                button->pressed    = false;
                button->elapsed_ms = 0U;
                if (button->release_from_long_press)
                {
                    button->release_from_long_press = false;
                    button->state                   = BUTTON_DRIVER_STATE_IDLE;
                    button_driver_emit(button, BUTTON_DRIVER_EVENT_LONG_PRESS_END);
                }
                else
                {
                    if (button->click_count < button->config.max_click_count)
                    {
                        ++button->click_count;
                    }
                    button->state = BUTTON_DRIVER_STATE_WAIT_NEXT_CLICK;
                    button_driver_emit(button, BUTTON_DRIVER_EVENT_RELEASE);
                }
            }
            break;

        case BUTTON_DRIVER_STATE_PRESSED:
            if (!button_driver_level_is_active(button))
            {
                button->elapsed_ms              = 0U;
                button->release_from_long_press = false;
                button->state                   = BUTTON_DRIVER_STATE_DEBOUNCE_RELEASE;
                break;
            }

            button_driver_advance_elapsed(button, elapsed_ms);
            if (button->elapsed_ms >= button->config.long_press_ms)
            {
                button->click_count = 0U;
                button->elapsed_ms  = 0U;
                button->state       = BUTTON_DRIVER_STATE_LONG_PRESS;
                button_driver_emit(button, BUTTON_DRIVER_EVENT_LONG_PRESS_START);
            }
            break;

        case BUTTON_DRIVER_STATE_WAIT_NEXT_CLICK:
            if (button_driver_level_is_active(button))
            {
                button->elapsed_ms = 0U;
                button->state      = BUTTON_DRIVER_STATE_DEBOUNCE_PRESS;
                break;
            }

            button_driver_advance_elapsed(button, elapsed_ms);
            if (button->elapsed_ms >= button->config.multi_click_window_ms)
            {
                if (button->click_count == 1U)
                {
                    button_driver_emit(button, BUTTON_DRIVER_EVENT_CLICK);
                }
                else if (button->click_count == 2U)
                {
                    button_driver_emit(button, BUTTON_DRIVER_EVENT_DOUBLE_CLICK);
                }
                else if (button->click_count > 2U)
                {
                    button_driver_emit(button, BUTTON_DRIVER_EVENT_MULTI_CLICK);
                }
                button->click_count = 0U;
                button->elapsed_ms  = 0U;
                button->state       = BUTTON_DRIVER_STATE_IDLE;
            }
            break;

        case BUTTON_DRIVER_STATE_LONG_PRESS:
            if (!button_driver_level_is_active(button))
            {
                button->elapsed_ms              = 0U;
                button->release_from_long_press = true;
                button->state                   = BUTTON_DRIVER_STATE_DEBOUNCE_RELEASE;
                break;
            }

            button_driver_advance_elapsed(button, elapsed_ms);
            if (button->elapsed_ms >= button->config.long_press_hold_ms)
            {
                button->elapsed_ms = 0U;
                button_driver_emit(button, BUTTON_DRIVER_EVENT_LONG_PRESS_HOLD);
            }
            break;

        default:
            button->pressed    = false;
            button->elapsed_ms = 0U;
            button->state      = BUTTON_DRIVER_STATE_IDLE;
            break;
    }

    return ESP_OK;
}

esp_err_t button_driver_is_pressed(const button_driver_t *button, bool *out_pressed)
{
    if (button == NULL || out_pressed == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!button->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    *out_pressed = button->pressed;
    return ESP_OK;
}

esp_err_t button_driver_deinit(button_driver_t *button)
{
    if (button == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!button->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(button, 0, sizeof(*button));
    return ESP_OK;
}
