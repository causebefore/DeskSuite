/**
 * @file device_power.c
 * @brief 把设备轻睡眠能力稳定转发到当前板型 BSP
 */
#include "device_power.h"

#include "bsp.h"

esp_err_t device_power_enter_light_sleep(uint32_t timer_wakeup_ms, device_power_wakeup_info_t *out_wakeup)
{
    if (timer_wakeup_ms == 0U || out_wakeup == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_power_wakeup_info_t bsp_wakeup = { 0 };
    const esp_err_t         error      = bsp_power_enter_light_sleep(timer_wakeup_ms, &bsp_wakeup);
    if (error == ESP_OK)
    {
        *out_wakeup = (device_power_wakeup_info_t) {
            .left_button  = bsp_wakeup.left_button,
            .right_button = bsp_wakeup.right_button,
            .timer        = bsp_wakeup.timer,
        };
    }
    return error;
}
