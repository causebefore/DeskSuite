/**
 * @file pcf8563.h
 * @brief NXP PCF8563 实时时钟 I2C 驱动
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

    /** @brief PCF8563 驱动实例，由 BSP 提供静态存储 */
    typedef struct
    {
        i2c_master_dev_handle_t device;
        bool                    initialized;
    } pcf8563_t;

    /** @brief 需要写入 PCF8563 的日历时间 */
    typedef struct
    {
        uint16_t year;
        uint8_t  month;
        uint8_t  day;
        uint8_t  hour;
        uint8_t  minute;
        uint8_t  second;
    } pcf8563_datetime_t;

    /** @brief 从 PCF8563 复制出的完整时间快照 */
    typedef struct
    {
        pcf8563_datetime_t datetime;
        uint8_t            weekday;
        bool               voltage_low;
    } pcf8563_snapshot_t;

    /**
     * @brief 在指定 I2C 总线上初始化 PCF8563
     *
     * 初始化会清除 STOP 位和报警/定时器控制位，并关闭未使用的 CLKOUT；不会修改日历时间。
     *
     * @param[out] out_rtc 调用方提供且已清零的驱动实例
     * @param[in] bus 已由 BSP 创建并复位的 I2C master bus
     * @param[in] address_7bit PCF8563 的 7 位地址
     * @param[in] scl_speed_hz I2C 时钟频率
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NOT_FOUND 器件无响应；
     *         或底层 I2C 错误码
     */
    esp_err_t pcf8563_init(pcf8563_t *out_rtc, i2c_master_bus_handle_t bus,
                           uint16_t address_7bit, uint32_t scl_speed_hz);

    /**
     * @brief 读取当前时间和电压过低标志
     *
     * @param[in] rtc 已初始化的驱动实例
     * @param[out] out_snapshot 时间快照，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE 寄存器内容无效；或底层错误码
     */
    esp_err_t pcf8563_get_snapshot_copy(const pcf8563_t *rtc,
                                        pcf8563_snapshot_t *out_snapshot);

    /**
     * @brief 单独读取电压过低标志
     *
     * 即使其余日历寄存器内容尚无效，本函数仍可用于判断是否需要首次校时。
     *
     * @param[in] rtc 已初始化的驱动实例
     * @param[out] out_voltage_low true 表示 RTC 时间不可信，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或底层错误码
     */
    esp_err_t pcf8563_get_voltage_low(const pcf8563_t *rtc, bool *out_voltage_low);

    /**
     * @brief 写入 2000 至 2099 年的日历时间并自动计算星期
     *
     * 成功写入秒寄存器会清除 VL 标志。
     *
     * @param[in] rtc 已初始化的驱动实例
     * @param[in] datetime 待写入时间，仅在调用期间借用
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 日期无效；或底层错误码
     */
    esp_err_t pcf8563_set_datetime(const pcf8563_t *rtc,
                                   const pcf8563_datetime_t *datetime);

    /**
     * @brief 从 I2C 总线移除 PCF8563 并恢复未初始化状态
     *
     * @param[in,out] rtc 已初始化的驱动实例
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或底层错误码
     */
    esp_err_t pcf8563_deinit(pcf8563_t *rtc);

#ifdef __cplusplus
}
#endif
