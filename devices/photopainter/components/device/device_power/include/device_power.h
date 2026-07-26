/**
 * @file device_power.h
 * @brief 与 GPIO 和芯片型号无关的设备深睡能力
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
 * @brief 准备由任意按键与可选内部定时器唤醒的深睡配置
 *
 * 调用前应停止按键扫描并确认用户已经释放左、右和确认按键。成功后若其他停机步骤失败，必须调用
 * device_power_cancel_deep_sleep() 回滚。
 *
 * @param[in] timer_wakeup_us 定时唤醒间隔，单位微秒；0 表示仅允许按键唤醒
 * @return ESP_OK 配置完成；ESP_ERR_INVALID_STATE 任意按键仍按下或重复准备；或底层错误码
 */
    esp_err_t device_power_prepare_deep_sleep(uint64_t timer_wakeup_us);

    /**
 * @brief 取消尚未进入的深睡准备
 *
 * @return ESP_OK 已取消；或底层错误码
 */
    esp_err_t device_power_cancel_deep_sleep(void);

    /**
 * @brief 进入已经准备好的深睡
 *
 * 本函数成功时不会返回；左、右、确认按键或定时器唤醒后芯片复位并重新执行 app_main()。
 */
    void device_power_start_deep_sleep(void) __attribute__((noreturn));

    /**
 * @brief 判断本次启动是否由任意按键从深睡唤醒
 *
 * @param[out] out_woken_by_button true 表示本次由左、右或确认按键唤醒
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出参数为空
 */
    esp_err_t device_power_was_woken_by_button(bool *out_woken_by_button);

    /**
 * @brief 判断本次启动是否由内部定时器从深睡唤醒
 *
 * @param[out] out_woken_by_timer true 表示本次由内部定时器唤醒
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出参数为空
 */
    esp_err_t device_power_was_woken_by_timer(bool *out_woken_by_timer);

#ifdef __cplusplus
}
#endif
