/**
 * @file pcf8563.c
 * @brief PCF8563 寄存器访问、BCD 编解码与日期校验
 */
#include "pcf8563.h"

#include <stddef.h>

/** @brief PCF8563 Control_status_1 寄存器 */
#define PCF8563_REG_CONTROL_STATUS_1 0x00U
/** @brief PCF8563 Control_status_2 寄存器 */
#define PCF8563_REG_CONTROL_STATUS_2 0x01U
/** @brief PCF8563 VL_seconds 起始寄存器 */
#define PCF8563_REG_SECONDS          0x02U
/** @brief PCF8563 CLKOUT_control 寄存器 */
#define PCF8563_REG_CLKOUT_CONTROL   0x0DU
/** @brief 单次 I2C 事务超时 */
#define PCF8563_I2C_TIMEOUT_MS       100

static uint8_t pcf8563_decimal_to_bcd(uint8_t value)
{
    return (uint8_t) (((value / 10U) << 4U) | (value % 10U));
}

/** @brief 校验一个带掩码的 BCD 寄存器并转换为十进制 */
static esp_err_t pcf8563_bcd_to_decimal(uint8_t raw, uint8_t mask, uint8_t maximum,
                                        uint8_t *out_value)
{
    const uint8_t bcd  = raw & mask;
    const uint8_t high = (bcd >> 4U) & 0x0FU;
    const uint8_t low  = bcd & 0x0FU;
    const uint8_t value = (uint8_t) (high * 10U + low);
    if (low > 9U || value > maximum)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_value = value;
    return ESP_OK;
}

static bool pcf8563_is_leap_year(uint16_t year)
{
    return (year % 4U) == 0U && ((year % 100U) != 0U || (year % 400U) == 0U);
}

static uint8_t pcf8563_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {31U, 28U, 31U, 30U, 31U, 30U,
                                   31U, 31U, 30U, 31U, 30U, 31U};
    if (month == 2U && pcf8563_is_leap_year(year))
    {
        return 29U;
    }
    return days[month - 1U];
}

/** @brief 校验驱动支持的 2000 至 2099 年完整日历时间 */
static bool pcf8563_datetime_is_valid(const pcf8563_datetime_t *datetime)
{
    if (datetime == NULL || datetime->year < 2000U || datetime->year > 2099U
        || datetime->month < 1U || datetime->month > 12U || datetime->day < 1U
        || datetime->hour > 23U || datetime->minute > 59U || datetime->second > 59U)
    {
        return false;
    }
    return datetime->day <= pcf8563_days_in_month(datetime->year, datetime->month);
}

/** @brief 使用 Sakamoto 算法计算星期，返回 0=星期日 至 6=星期六 */
static uint8_t pcf8563_calculate_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    static const uint8_t offsets[] = {0U, 3U, 2U, 5U, 0U, 3U,
                                      5U, 1U, 4U, 6U, 2U, 4U};
    uint16_t adjusted_year = year;
    if (month < 3U)
    {
        --adjusted_year;
    }
    return (uint8_t) ((adjusted_year + adjusted_year / 4U - adjusted_year / 100U
                       + adjusted_year / 400U + offsets[month - 1U] + day)
                      % 7U);
}

static esp_err_t pcf8563_write_register(const pcf8563_t *rtc, uint8_t reg, uint8_t value)
{
    const uint8_t payload[] = {reg, value};
    return i2c_master_transmit(rtc->device, payload, sizeof(payload), PCF8563_I2C_TIMEOUT_MS);
}

esp_err_t pcf8563_init(pcf8563_t *out_rtc, i2c_master_bus_handle_t bus,
                       uint16_t address_7bit, uint32_t scl_speed_hz)
{
    if (out_rtc == NULL || bus == NULL || address_7bit > 0x7FU || scl_speed_hz == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_rtc->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = i2c_master_probe(bus, address_7bit, PCF8563_I2C_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        return error;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address_7bit,
        .scl_speed_hz    = scl_speed_hz,
    };
    error = i2c_master_bus_add_device(bus, &device_config, &out_rtc->device);
    if (error != ESP_OK)
    {
        return error;
    }

    error = pcf8563_write_register(out_rtc, PCF8563_REG_CONTROL_STATUS_1, 0x00U);
    if (error == ESP_OK)
    {
        error = pcf8563_write_register(out_rtc, PCF8563_REG_CONTROL_STATUS_2, 0x00U);
    }
    if (error == ESP_OK)
    {
        error = pcf8563_write_register(out_rtc, PCF8563_REG_CLKOUT_CONTROL, 0x00U);
    }
    if (error != ESP_OK)
    {
        (void) i2c_master_bus_rm_device(out_rtc->device);
        out_rtc->device = NULL;
        return error;
    }

    out_rtc->initialized = true;
    return ESP_OK;
}

esp_err_t pcf8563_get_snapshot_copy(const pcf8563_t *rtc,
                                    pcf8563_snapshot_t *out_snapshot)
{
    if (rtc == NULL || out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rtc->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t start_register = PCF8563_REG_SECONDS;
    uint8_t raw[7];
    esp_err_t error = i2c_master_transmit_receive(rtc->device, &start_register,
                                                   sizeof(start_register), raw, sizeof(raw),
                                                   PCF8563_I2C_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        return error;
    }

    pcf8563_snapshot_t snapshot = {
        .voltage_low = (raw[0] & 0x80U) != 0U,
    };
    error = pcf8563_bcd_to_decimal(raw[0], 0x7FU, 59U, &snapshot.datetime.second);
    if (error == ESP_OK)
    {
        error = pcf8563_bcd_to_decimal(raw[1], 0x7FU, 59U, &snapshot.datetime.minute);
    }
    if (error == ESP_OK)
    {
        error = pcf8563_bcd_to_decimal(raw[2], 0x3FU, 23U, &snapshot.datetime.hour);
    }
    if (error == ESP_OK)
    {
        error = pcf8563_bcd_to_decimal(raw[3], 0x3FU, 31U, &snapshot.datetime.day);
    }
    if (error == ESP_OK)
    {
        error = pcf8563_bcd_to_decimal(raw[5], 0x1FU, 12U, &snapshot.datetime.month);
    }

    uint8_t year_in_century = 0U;
    if (error == ESP_OK)
    {
        error = pcf8563_bcd_to_decimal(raw[6], 0xFFU, 99U, &year_in_century);
    }
    snapshot.weekday = raw[4] & 0x07U;
    if (error != ESP_OK || snapshot.weekday > 6U)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    snapshot.datetime.year = (raw[5] & 0x80U) != 0U ? (uint16_t) (1900U + year_in_century)
                                                   : (uint16_t) (2000U + year_in_century);
    if (snapshot.datetime.day < 1U || snapshot.datetime.month < 1U
        || snapshot.datetime.day
               > pcf8563_days_in_month(snapshot.datetime.year, snapshot.datetime.month))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_snapshot = snapshot;
    return ESP_OK;
}

esp_err_t pcf8563_get_voltage_low(const pcf8563_t *rtc, bool *out_voltage_low)
{
    if (rtc == NULL || out_voltage_low == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rtc->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t register_address = PCF8563_REG_SECONDS;
    uint8_t seconds;
    const esp_err_t error = i2c_master_transmit_receive(
        rtc->device, &register_address, sizeof(register_address), &seconds, sizeof(seconds),
        PCF8563_I2C_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        return error;
    }
    *out_voltage_low = (seconds & 0x80U) != 0U;
    return ESP_OK;
}

esp_err_t pcf8563_set_datetime(const pcf8563_t *rtc,
                               const pcf8563_datetime_t *datetime)
{
    if (rtc == NULL || !pcf8563_datetime_is_valid(datetime))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rtc->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t payload[] = {
        PCF8563_REG_SECONDS,
        pcf8563_decimal_to_bcd(datetime->second),
        pcf8563_decimal_to_bcd(datetime->minute),
        pcf8563_decimal_to_bcd(datetime->hour),
        pcf8563_decimal_to_bcd(datetime->day),
        pcf8563_calculate_weekday(datetime->year, datetime->month, datetime->day),
        pcf8563_decimal_to_bcd(datetime->month),
        pcf8563_decimal_to_bcd((uint8_t) (datetime->year % 100U)),
    };
    return i2c_master_transmit(rtc->device, payload, sizeof(payload), PCF8563_I2C_TIMEOUT_MS);
}

esp_err_t pcf8563_deinit(pcf8563_t *rtc)
{
    if (rtc == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rtc->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = i2c_master_bus_rm_device(rtc->device);
    if (error != ESP_OK)
    {
        return error;
    }
    rtc->device      = NULL;
    rtc->initialized = false;
    return ESP_OK;
}
