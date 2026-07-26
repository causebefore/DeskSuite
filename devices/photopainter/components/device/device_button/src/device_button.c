/**
 * @file device_button.c
 * @brief 把 BSP 物理按键事件收敛为稳定的 Device API
 */
#include "device_button.h"

#include <stddef.h>

#include "bsp.h"
#include "esp_check.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "device_button";

/** @brief Device 与 BSP 按键 ID 直接转换所依赖的编译期映射约束 */
_Static_assert((int) DEVICE_BUTTON_LEFT == (int) BSP_BUTTON_LEFT, "左键 ID 映射不一致");
_Static_assert((int) DEVICE_BUTTON_RIGHT == (int) BSP_BUTTON_RIGHT, "右键 ID 映射不一致");
_Static_assert((int) DEVICE_BUTTON_CONFIRM == (int) BSP_BUTTON_CONFIRM, "确认键 ID 映射不一致");
_Static_assert((int) DEVICE_BUTTON_COUNT == (int) BSP_BUTTON_COUNT, "按键数量映射不一致");

/** @brief 上层借用的事件回调 */
static device_button_event_cb_t s_event_callback;

/** @brief 上层借用的事件上下文 */
static void *s_event_context;

/** @brief Device 按键生命周期状态 */
static bool s_initialized;

/** @brief 把 BSP 事件翻译为不含板级类型的 Device 事件 */
static void device_button_on_bsp_event(bsp_button_id_t button, bsp_button_event_t event,
                                       uint8_t click_count, void *context)
{
    (void) context;
    if (s_event_callback != NULL)
    {
        s_event_callback((device_button_id_t) button,
                         (device_button_event_t) event,
                         click_count,
                         s_event_context);
    }
}

esp_err_t device_button_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(bsp_buttons_init(), TAG, "BSP 按键初始化失败");
    s_initialized = true;
    ESP_LOGI(TAG, "设备按键能力初始化完成");
    return ESP_OK;
}

esp_err_t device_button_set_event_callback_borrow(device_button_event_cb_t callback, void *context)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        bsp_buttons_set_event_callback_borrow(callback != NULL ? device_button_on_bsp_event : NULL,
                                              NULL),
        TAG,
        "BSP 按键回调设置失败");
    s_event_callback = callback;
    s_event_context  = callback != NULL ? context : NULL;
    return ESP_OK;
}

esp_err_t device_button_scan(uint32_t elapsed_ms)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (elapsed_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return bsp_buttons_scan(elapsed_ms);
}

esp_err_t device_button_is_pressed(device_button_id_t button, bool *out_pressed)
{
    if (button < DEVICE_BUTTON_LEFT || button >= DEVICE_BUTTON_COUNT || out_pressed == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return bsp_buttons_is_pressed((bsp_button_id_t) button, out_pressed);
}

esp_err_t device_button_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(bsp_buttons_deinit(), TAG, "BSP 按键资源释放失败");
    s_event_callback = NULL;
    s_event_context  = NULL;
    s_initialized    = false;
    return ESP_OK;
}
