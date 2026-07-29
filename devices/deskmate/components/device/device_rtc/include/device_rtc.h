/**
 * @file device_rtc.h
 * @brief 与 RTC 型号、I2C 和 Board 参数无关的设备日历时钟能力
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief RTC 日历时间 */
    typedef struct
    {
        uint16_t year;
        uint8_t  month;
        uint8_t  day;
        uint8_t  hour;
        uint8_t  minute;
        uint8_t  second;
    } device_rtc_datetime_t;

    /** @brief RTC 当前时间和可信状态快照 */
    typedef struct
    {
        device_rtc_datetime_t datetime;
        uint8_t               weekday;
        bool                  voltage_low;
    } device_rtc_snapshot_t;

    /** @brief RTC 告警参与匹配的字段 */
    typedef enum
    {
        DEVICE_RTC_ALARM_MATCH_SECOND  = 1U << 0,
        DEVICE_RTC_ALARM_MATCH_MINUTE  = 1U << 1,
        DEVICE_RTC_ALARM_MATCH_HOUR    = 1U << 2,
        DEVICE_RTC_ALARM_MATCH_DAY     = 1U << 3,
        DEVICE_RTC_ALARM_MATCH_WEEKDAY = 1U << 4,
        DEVICE_RTC_ALARM_MATCH_ALL     = (1U << 5) - 1U,
    } device_rtc_alarm_match_t;

    /**
     * @brief RTC 告警比较配置
     *
     * `match_fields` 中置位的字段参与比较。至少启用一个字段；未启用字段的数值会被忽略。
     */
    typedef struct
    {
        uint8_t second;       /**< 秒，范围 0—59 */
        uint8_t minute;       /**< 分，范围 0—59 */
        uint8_t hour;         /**< 时，范围 0—23 */
        uint8_t day;          /**< 日，范围 1—31 */
        uint8_t weekday;      /**< 星期，范围 0—6，0 表示星期日 */
        uint8_t match_fields; /**< `device_rtc_alarm_match_t` 位组合 */
    } device_rtc_alarm_t;

    /**
     * @brief RTC INT 快速通知回调
     *
     * 回调在 GPIO ISR 上下文执行，只能调用 ISR-safe API，必须快速返回。
     */
    typedef void (*device_rtc_interrupt_callback_t)(void *context);

    /**
     * @brief 初始化设备 RTC 能力但不修改日历时间
     * @return ESP_OK 成功，或 BSP/Driver 初始化错误
     */
    esp_err_t device_rtc_init(void);

    /**
     * @brief 从硬件读取设备 RTC 当前时间和可信状态快照
     *
     * 本函数执行 I2C 读取，不是内存副本 Getter。
     * @param[out] out_snapshot 快照，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t device_rtc_read_snapshot(device_rtc_snapshot_t *out_snapshot);

    /**
     * @brief 单独读取设备 RTC 的电压过低状态
     * @param[out] out_voltage_low true 表示 RTC 时间不可信，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t device_rtc_read_voltage_low(bool *out_voltage_low);

    /**
     * @brief 写入设备 RTC 日历时间
     * @param[in] datetime 日历时间，仅在调用期间借用
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或时间无效；或底层错误码
     */
    esp_err_t device_rtc_set_datetime(const device_rtc_datetime_t *datetime);

    /**
     * @brief 配置 RTC 告警、清除旧标志并启用告警中断输出
     * @param[in] alarm 告警比较配置，仅在调用期间借用
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或字段无效；或底层错误码
     */
    esp_err_t device_rtc_set_alarm(const device_rtc_alarm_t *alarm);

    /**
     * @brief 读取 RTC 告警比较配置
     * @param[out] out_alarm 告警配置，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t device_rtc_read_alarm(device_rtc_alarm_t *out_alarm);

    /**
     * @brief 启用或关闭 RTC 告警中断输出
     * @param[in] enabled true 启用，false 关闭
     * @return ESP_OK 成功，或底层错误码
     */
    esp_err_t device_rtc_enable_alarm_interrupt(bool enabled);

    /**
     * @brief 读取 RTC 告警中断输出是否启用
     * @param[out] out_enabled true 表示已启用，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t device_rtc_read_alarm_interrupt_enabled(bool *out_enabled);

    /**
     * @brief 读取 RTC 告警标志
     * @param[out] out_pending true 表示 AF 已置位，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t device_rtc_read_alarm_flag(bool *out_pending);

    /**
     * @brief 清除 RTC 告警标志并释放低电平 INT
     * @return ESP_OK 成功，或底层错误码
     */
    esp_err_t device_rtc_clear_alarm_flag(void);

    /**
     * @brief 采样并诊断 RTC INT 是否处于低电平有效状态
     *
     * 无真实中断来源时，底层可能短暂关闭并恢复 AIE，以区分 RTC 输出门控与板级拉低；
     * 不会清除 AF 或 TF。
     *
     * @param[out] out_asserted true 表示诊断完成后 INT 仍有效，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t device_rtc_read_interrupt_asserted(bool *out_asserted);

    /**
     * @brief 设置长期借用的 RTC INT ISR 回调
     *
     * 传入 NULL 清除回调。借用期持续到再次设置、清除或 `device_rtc_deinit()`。
     *
     * @param[in] callback ISR 回调，可为 NULL
     * @param[in] context 原样传给回调的上下文
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE RTC 尚未初始化
     */
    esp_err_t device_rtc_set_interrupt_callback_borrow(device_rtc_interrupt_callback_t callback, void *context);

    /**
     * @brief 释放设备 RTC 能力
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或底层错误码
     */
    esp_err_t device_rtc_deinit(void);

#ifdef __cplusplus
}
#endif
