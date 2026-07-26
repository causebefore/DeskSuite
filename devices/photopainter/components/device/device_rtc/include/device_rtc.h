/**
 * @file device_rtc.h
 * @brief 与 RTC 型号、总线和 GPIO 无关的设备日历时钟能力
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 需要写入设备 RTC 的日历时间 */
    typedef struct
    {
        uint16_t year;
        uint8_t  month;
        uint8_t  day;
        uint8_t  hour;
        uint8_t  minute;
        uint8_t  second;
    } device_rtc_datetime_t;

    /** @brief 从设备 RTC 复制出的完整时间快照 */
    typedef struct
    {
        device_rtc_datetime_t datetime;
        uint8_t               weekday;
        bool                  voltage_low;
    } device_rtc_snapshot_t;

    /**
     * @brief 初始化设备 RTC 能力但不修改日历时间
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或 BSP 错误码
     */
    esp_err_t device_rtc_init(void);

    /**
     * @brief 复制设备 RTC 当前时间和电压过低状态
     *
     * @param[out] out_snapshot RTC 快照，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或底层错误码
     */
    esp_err_t device_rtc_get_snapshot_copy(device_rtc_snapshot_t *out_snapshot);

    /**
     * @brief 单独读取设备 RTC 的电压过低标志
     *
     * @param[out] out_voltage_low true 表示 RTC 时间不可信，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或底层错误码
     */
    esp_err_t device_rtc_get_voltage_low(bool *out_voltage_low);

    /**
     * @brief 写入 2000 至 2099 年的设备 RTC 日历时间
     *
     * 星期由底层根据日期自动计算；成功写入会清除电压过低标志。
     *
     * @param[in] datetime 有效日历时间，仅在调用期间借用
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 日期无效；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或底层错误码
     */
    esp_err_t device_rtc_set_datetime(const device_rtc_datetime_t *datetime);

    /**
     * @brief 释放设备 RTC 能力
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
     */
    esp_err_t device_rtc_deinit(void);

#ifdef __cplusplus
}
#endif
