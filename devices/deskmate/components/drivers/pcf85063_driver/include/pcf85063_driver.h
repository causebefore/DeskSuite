/**
 * @file pcf85063_driver.h
 * @brief PCF85063 芯片协议，不包含板级地址和业务策略
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        i2c_master_dev_handle_t i2c_device; /**< 由 BSP 创建并长期借用的 I2C Device */
        uint32_t                timeout_ms; /**< 单次寄存器事务超时，单位毫秒 */
    } pcf85063_driver_t;

    /** @brief PCF85063 支持范围内的日历时间 */
    typedef struct
    {
        uint16_t year;
        uint8_t  month;
        uint8_t  day;
        uint8_t  hour;
        uint8_t  minute;
        uint8_t  second;
    } pcf85063_datetime_t;

    /** @brief PCF85063 告警参与匹配的字段 */
    typedef enum
    {
        PCF85063_ALARM_MATCH_SECOND  = 1U << 0,
        PCF85063_ALARM_MATCH_MINUTE  = 1U << 1,
        PCF85063_ALARM_MATCH_HOUR    = 1U << 2,
        PCF85063_ALARM_MATCH_DAY     = 1U << 3,
        PCF85063_ALARM_MATCH_WEEKDAY = 1U << 4,
        PCF85063_ALARM_MATCH_ALL     = (1U << 5) - 1U,
    } pcf85063_alarm_match_t;

    /**
     * @brief PCF85063 告警配置
     *
     * `match_fields` 中置位的字段参与硬件比较；未置位字段写入 AEN_x=1 并被忽略。
     */
    typedef struct
    {
        uint8_t second;       /**< 秒，范围 0—59 */
        uint8_t minute;       /**< 分，范围 0—59 */
        uint8_t hour;         /**< 时，范围 0—23 */
        uint8_t day;          /**< 日，范围 1—31 */
        uint8_t weekday;      /**< 星期，范围 0—6 */
        uint8_t match_fields; /**< `pcf85063_alarm_match_t` 位组合 */
    } pcf85063_alarm_t;

    /** @brief PCF85063 中断控制寄存器与标志的硬件快照 */
    typedef struct
    {
        uint8_t control2_raw;                  /**< Control_2 原始寄存器值 */
        uint8_t timer_mode_raw;                /**< Timer_mode 原始寄存器值 */
        bool    alarm_interrupt_enabled;       /**< AIE 已置位 */
        bool    alarm_flag;                    /**< AF 已置位 */
        bool    minute_interrupt_enabled;      /**< MI 已置位 */
        bool    half_minute_interrupt_enabled; /**< HMI 已置位 */
        bool    timer_flag;                    /**< TF 已置位 */
        bool    timer_enabled;                 /**< TE 已置位 */
        bool    timer_interrupt_enabled;       /**< TIE 已置位 */
    } pcf85063_interrupt_snapshot_t;

    /**
     * @brief 初始化 Driver，并关闭未支持的分钟与计时器中断源
     *
     * 初始化保留现有 AIE、AF、告警比较配置和 CLKOUT 配置，使启动前已经发生的有效告警仍可由
     * 上层消费；同时关闭 MI、HMI、TE、TIE 并清除遗留 TF，避免非告警来源持续拉低 INT。
     *
     * @param[out] driver Driver 实例
     * @param[in] i2c_device BSP 创建的 I2C Device，必须覆盖 Driver 使用期
     * @param[in] timeout_ms 单次 I2C 事务超时，单位毫秒
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；或 I2C 错误
     */
    esp_err_t pcf85063_driver_init(pcf85063_driver_t *driver, i2c_master_dev_handle_t i2c_device, uint32_t timeout_ms);

    /**
     * @brief 读取 RTC 中断控制与标志快照
     *
     * Control_2 与 Timer_mode 按顺序读取；本函数不修改任何寄存器。
     *
     * @param[in] driver Driver 实例
     * @param[out] out_snapshot 中断寄存器快照，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；或 I2C 错误
     */
    esp_err_t pcf85063_driver_read_interrupt_snapshot(pcf85063_driver_t             *driver,
                                                      pcf85063_interrupt_snapshot_t *out_snapshot);

    /**
     * @brief 读取并解码 RTC 日历寄存器
     * @param[in] driver Driver 实例
     * @param[out] out 日历时间，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE 寄存器值非法；或 I2C 错误
     */
    esp_err_t pcf85063_driver_read_datetime(pcf85063_driver_t *driver, pcf85063_datetime_t *out);

    /**
     * @brief 读取振荡停止/电压过低标志
     * @param[in] driver Driver 实例
     * @param[out] out_voltage_low true 表示 RTC 时间不可信
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；或 I2C 错误
     */
    esp_err_t pcf85063_driver_read_voltage_low(pcf85063_driver_t *driver, bool *out_voltage_low);

    /**
     * @brief 校验并写入 RTC 日历寄存器
     * @param[in] driver Driver 实例
     * @param[in] value 日历时间，仅在调用期间借用
     */
    esp_err_t pcf85063_driver_set_datetime(pcf85063_driver_t *driver, const pcf85063_datetime_t *value);

    /**
     * @brief 写入告警比较字段、清除旧 AF 并启用 AIE
     * @param[in] driver Driver 实例
     * @param[in] alarm 告警配置，仅在调用期间借用，至少启用一个匹配字段
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或字段无效；或 I2C 错误
     */
    esp_err_t pcf85063_driver_set_alarm(pcf85063_driver_t *driver, const pcf85063_alarm_t *alarm);

    /**
     * @brief 读取告警比较字段
     * @param[in] driver Driver 实例
     * @param[out] out_alarm 告警配置，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE 寄存器值非法；或 I2C 错误
     */
    esp_err_t pcf85063_driver_read_alarm(pcf85063_driver_t *driver, pcf85063_alarm_t *out_alarm);

    /**
     * @brief 启用或关闭 RTC 告警中断输出
     *
     * 只修改 AIE，保留调用期间可能由硬件新置位的 AF 和 TF。
     *
     * @param[in] driver Driver 实例
     * @param[in] enabled true 启用 AIE，false 关闭 AIE
     * @return ESP_OK 成功，或 I2C 错误
     */
    esp_err_t pcf85063_driver_enable_alarm_interrupt(pcf85063_driver_t *driver, bool enabled);

    /**
     * @brief 读取 RTC 告警中断输出是否已启用
     * @param[in] driver Driver 实例
     * @param[out] out_enabled true 表示 AIE 已置位，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；或 I2C 错误
     */
    esp_err_t pcf85063_driver_read_alarm_interrupt_enabled(pcf85063_driver_t *driver, bool *out_enabled);

    /**
     * @brief 读取 RTC 告警标志
     * @param[in] driver Driver 实例
     * @param[out] out_pending true 表示 AF 已置位，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；或 I2C 错误
     */
    esp_err_t pcf85063_driver_read_alarm_flag(pcf85063_driver_t *driver, bool *out_pending);

    /**
     * @brief 清除 RTC 告警标志
     *
     * 只清除 AF，保留调用期间可能由硬件新置位的 TF。
     *
     * @param[in] driver Driver 实例
     * @return ESP_OK 成功，或 I2C 错误
     */
    esp_err_t pcf85063_driver_clear_alarm_flag(pcf85063_driver_t *driver);

#ifdef __cplusplus
}
#endif
