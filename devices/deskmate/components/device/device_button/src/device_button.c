/* 文件职责：封装双按键 BSP 读取，并实现消抖和长短按状态机。 */
#include "device_button.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "bsp.h"
#include "esp_check.h"

typedef struct
{
    bool     candidate_pressed;
    bool     stable_pressed;
    bool     long_sent;
    uint32_t candidate_changed_ms;
    uint32_t pressed_ms;
} device_button_state_t;

typedef struct
{
    device_button_state_t states[BSP_BUTTON_COUNT];
    uint32_t              debounce_ms;
    uint32_t              long_press_ms;
    bool                  initialized;
} device_button_context_t;

static const char             *TAG = "device_button";
static device_button_context_t s_context;

/** @brief 判断单键状态机是否仍处于消抖或长按判定窗口 */
static bool button_follow_up_required(const device_button_state_t *state)
{
    return state->candidate_pressed != state->stable_pressed || (state->stable_pressed && !state->long_sent);
}

static device_button_event_t make_short_event(bsp_button_id_t button)
{
    return button == BSP_BUTTON_LEFT ? DEVICE_BUTTON_EVENT_LEFT_SHORT : DEVICE_BUTTON_EVENT_RIGHT_SHORT;
}

static device_button_event_t make_long_event(bsp_button_id_t button)
{
    return button == BSP_BUTTON_LEFT ? DEVICE_BUTTON_EVENT_LEFT_LONG : DEVICE_BUTTON_EVENT_RIGHT_LONG;
}

static device_button_event_t update_button(bsp_button_id_t button, device_button_state_t *state, bool raw_high,
                                           uint32_t now_ms)
{
    const bool pressed = !raw_high;
    if (pressed != state->candidate_pressed)
    {
        state->candidate_pressed    = pressed;
        state->candidate_changed_ms = now_ms;
        return DEVICE_BUTTON_EVENT_NONE;
    }

    if (pressed != state->stable_pressed && now_ms - state->candidate_changed_ms >= s_context.debounce_ms)
    {
        state->stable_pressed = pressed;
        if (pressed)
        {
            state->pressed_ms = now_ms;
            state->long_sent  = false;
        }
        else if (!state->long_sent)
        {
            return make_short_event(button);
        }
    }

    if (state->stable_pressed && !state->long_sent && now_ms - state->pressed_ms >= s_context.long_press_ms)
    {
        state->long_sent = true;
        return make_long_event(button);
    }
    return DEVICE_BUTTON_EVENT_NONE;
}

esp_err_t device_button_init(uint32_t debounce_ms, uint32_t long_press_ms)
{
    if (s_context.initialized)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(debounce_ms > 0 && long_press_ms > debounce_ms, ESP_ERR_INVALID_ARG, TAG, "按键时间参数无效");
    ESP_RETURN_ON_ERROR(bsp_button_init(), TAG, "初始化按键 BSP 失败");
    s_context.debounce_ms   = debounce_ms;
    s_context.long_press_ms = long_press_ms;
    s_context.initialized   = true;
    return ESP_OK;
}

esp_err_t device_button_scan(uint32_t now_ms, device_button_scan_result_t *out_result)
{
    ESP_RETURN_ON_FALSE(out_result != NULL, ESP_ERR_INVALID_ARG, TAG, "按键扫描结果输出为空");
    memset(out_result, 0, sizeof(*out_result));
    ESP_RETURN_ON_FALSE(s_context.initialized, ESP_ERR_INVALID_STATE, TAG, "按键能力未初始化");

    for (int button_value = BSP_BUTTON_LEFT; button_value < BSP_BUTTON_COUNT; ++button_value)
    {
        const bsp_button_id_t button   = (bsp_button_id_t) button_value;
        bool                  raw_high = false;
        ESP_RETURN_ON_ERROR(bsp_button_read_level(button, &raw_high), TAG, "读取按键电平失败");
        const device_button_event_t event = update_button(button, &s_context.states[button], raw_high, now_ms);
        if (event != DEVICE_BUTTON_EVENT_NONE)
        {
            out_result->events[out_result->event_count++] = event;
        }
    }
    out_result->follow_up_required = button_follow_up_required(&s_context.states[BSP_BUTTON_LEFT])
                                     || button_follow_up_required(&s_context.states[BSP_BUTTON_RIGHT]);
    return ESP_OK;
}

esp_err_t device_button_set_activity_callback_borrow(device_button_activity_callback_t callback, void *context)
{
    ESP_RETURN_ON_FALSE(s_context.initialized, ESP_ERR_INVALID_STATE, TAG, "按键能力未初始化");
    return bsp_button_set_activity_callback_borrow(callback, callback != NULL ? context : NULL);
}

esp_err_t device_button_read_pressed_snapshot(device_button_pressed_snapshot_t *out_snapshot)
{
    ESP_RETURN_ON_FALSE(out_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "按键快照输出为空");
    ESP_RETURN_ON_FALSE(s_context.initialized, ESP_ERR_INVALID_STATE, TAG, "按键能力未初始化");

    bool left_high  = false;
    bool right_high = false;
    ESP_RETURN_ON_ERROR(bsp_button_read_level(BSP_BUTTON_LEFT, &left_high), TAG, "读取左键电平失败");
    ESP_RETURN_ON_ERROR(bsp_button_read_level(BSP_BUTTON_RIGHT, &right_high), TAG, "读取右键电平失败");
    *out_snapshot = (device_button_pressed_snapshot_t) {
        .left_pressed  = !left_high,
        .right_pressed = !right_high,
    };
    return ESP_OK;
}

esp_err_t device_button_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_context.initialized, ESP_ERR_INVALID_STATE, TAG, "按键能力未初始化");
    ESP_RETURN_ON_ERROR(device_button_set_activity_callback_borrow(NULL, NULL), TAG, "清除按键活动回调失败");
    ESP_RETURN_ON_ERROR(bsp_button_deinit(), TAG, "释放按键 BSP 失败");
    s_context = (device_button_context_t) { 0 };
    return ESP_OK;
}
