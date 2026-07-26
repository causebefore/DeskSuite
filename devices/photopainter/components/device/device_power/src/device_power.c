/**
 * @file device_power.c
 * @brief 把板级按键与定时唤醒能力收敛为稳定 Device API
 */
#include "device_power.h"

#include "bsp.h"

esp_err_t device_power_prepare_deep_sleep(uint64_t timer_wakeup_us)
{
    return bsp_power_prepare_deep_sleep(timer_wakeup_us);
}

esp_err_t device_power_cancel_deep_sleep(void)
{
    return bsp_power_cancel_deep_sleep();
}

void device_power_start_deep_sleep(void)
{
    bsp_power_start_deep_sleep();
}

esp_err_t device_power_was_woken_by_button(bool *out_woken_by_button)
{
    return bsp_power_was_woken_by_button(out_woken_by_button);
}

esp_err_t device_power_was_woken_by_timer(bool *out_woken_by_timer)
{
    return bsp_power_was_woken_by_timer(out_woken_by_timer);
}
