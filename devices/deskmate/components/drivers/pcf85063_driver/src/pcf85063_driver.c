/*
 * 文件职责：实现 PCF85063 时间与闹钟寄存器协议。
 * 主要依赖：pcf85063_driver.h、ESP-IDF I2C master driver。
 * 调用方：板级 RTC 适配层。
 */
#include "pcf85063_driver.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pcf85063_driver";

#define PCF85063_REG_CTRL1         0x00
#define PCF85063_REG_CTRL2         0x01
#define PCF85063_REG_SECONDS       0x04
#define PCF85063_REG_ALARM_SECONDS 0x0B
#define PCF85063_REG_TIMER_MODE    0x11
#define PCF85063_ALARM_REG_COUNT   5U

#define PCF85063_CTRL2_AIE         (1U << 7)
#define PCF85063_CTRL2_AF          (1U << 6)
#define PCF85063_CTRL2_MI          (1U << 5)
#define PCF85063_CTRL2_HMI         (1U << 4)
#define PCF85063_CTRL2_TF          (1U << 3)
#define PCF85063_TIMER_MODE_TE     (1U << 2)
#define PCF85063_TIMER_MODE_TIE    (1U << 1)
#define PCF85063_SECONDS_OS        (1U << 7)
#define PCF85063_ALARM_DISABLE     (1U << 7)

static bool driver_is_valid(const pcf85063_driver_t *driver)
{
    return driver != NULL && driver->i2c_device != NULL && driver->timeout_ms > 0;
}

static bool datetime_is_valid(const pcf85063_datetime_t *value)
{
    return value != NULL && value->year >= 2000U && value->year <= 2099U && value->month >= 1U && value->month <= 12U
           && value->day >= 1U && value->day <= 31U && value->hour <= 23U && value->minute <= 59U
           && value->second <= 59U;
}

/** @brief 校验告警字段和值，至少要求一个字段参与匹配 */
static bool alarm_is_valid(const pcf85063_alarm_t *alarm)
{
    if (alarm == NULL || alarm->match_fields == 0U || (alarm->match_fields & (uint8_t) ~PCF85063_ALARM_MATCH_ALL) != 0U)
    {
        return false;
    }
    return ((alarm->match_fields & PCF85063_ALARM_MATCH_SECOND) == 0U || alarm->second <= 59U)
           && ((alarm->match_fields & PCF85063_ALARM_MATCH_MINUTE) == 0U || alarm->minute <= 59U)
           && ((alarm->match_fields & PCF85063_ALARM_MATCH_HOUR) == 0U || alarm->hour <= 23U)
           && ((alarm->match_fields & PCF85063_ALARM_MATCH_DAY) == 0U || (alarm->day >= 1U && alarm->day <= 31U))
           && ((alarm->match_fields & PCF85063_ALARM_MATCH_WEEKDAY) == 0U || alarm->weekday <= 6U);
}

/** @brief 根据公历日期计算星期，返回 0（星期日）至 6（星期六） */
static uint8_t calculate_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    static const uint8_t month_offsets[] = { 0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U };
    uint16_t             adjusted_year   = year;
    if (month < 3U)
    {
        --adjusted_year;
    }
    return (uint8_t) ((adjusted_year + adjusted_year / 4U - adjusted_year / 100U + adjusted_year / 400U
                       + month_offsets[month - 1U] + day)
                      % 7U);
}

static uint8_t bin_to_bcd(uint8_t value)
{
    return (uint8_t) (((value / 10U) << 4U) | (value % 10U));
}

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t) (((value >> 4U) * 10U) + (value & 0x0FU));
}

static esp_err_t read_register(pcf85063_driver_t *driver, uint8_t reg, uint8_t *value)
{
    if (!driver_is_valid(driver) || value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(driver->i2c_device, &reg, 1, value, 1, driver->timeout_ms);
}

static esp_err_t write_register(pcf85063_driver_t *driver, uint8_t reg, uint8_t value)
{
    if (!driver_is_valid(driver))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t data[] = { reg, value };
    return i2c_master_transmit(driver->i2c_device, data, sizeof(data), driver->timeout_ms);
}

/**
 * @brief 关闭当前 Driver 未支持的分钟与计时器中断源
 *
 * 先停止计时器，再清除 TF，避免清除期间重新置位；AIE、AF、COF 和告警比较配置保持不变。
 */
static esp_err_t normalize_unsupported_interrupt_sources(pcf85063_driver_t *driver)
{
    pcf85063_interrupt_status_t status;
    ESP_RETURN_ON_ERROR(pcf85063_driver_get_interrupt_status_copy(driver, &status), TAG, "读取 RTC 中断状态失败");

    const uint8_t unsupported_timer_mode = status.timer_mode_raw & (PCF85063_TIMER_MODE_TE | PCF85063_TIMER_MODE_TIE);
    const uint8_t normalized_timer_mode =
        status.timer_mode_raw & (uint8_t) ~(PCF85063_TIMER_MODE_TE | PCF85063_TIMER_MODE_TIE);
    if (unsupported_timer_mode != 0U)
    {
        ESP_RETURN_ON_ERROR(write_register(driver, PCF85063_REG_TIMER_MODE, normalized_timer_mode),
                            TAG,
                            "关闭 RTC 计时器中断源失败");
    }

    const uint8_t unsupported_control2 =
        status.control2_raw & (PCF85063_CTRL2_MI | PCF85063_CTRL2_HMI | PCF85063_CTRL2_TF);
    if (unsupported_control2 != 0U)
    {
        /*
         * AF/TF 写入采用“当前值 AND 写入值”语义：TF 写 0 清除；AF 必须强制写 1，
         * 才能保留状态读取后、寄存器写入前刚好置位的新告警。
         */
        const uint8_t control2_write_value =
            (status.control2_raw & (uint8_t) ~(PCF85063_CTRL2_MI | PCF85063_CTRL2_HMI | PCF85063_CTRL2_TF))
            | PCF85063_CTRL2_AF;
        ESP_RETURN_ON_ERROR(write_register(driver, PCF85063_REG_CTRL2, control2_write_value),
                            TAG,
                            "关闭 RTC 分钟中断并清除计时器标志失败");
    }

    if (unsupported_timer_mode != 0U || unsupported_control2 != 0U)
    {
        ESP_LOGW(TAG,
                 "已清理遗留 RTC 中断源: 原 Control_2=0x%02x, 原 Timer_mode=0x%02x",
                 status.control2_raw,
                 status.timer_mode_raw);
    }
    return ESP_OK;
}

esp_err_t pcf85063_driver_init(pcf85063_driver_t *driver, i2c_master_dev_handle_t i2c_device, uint32_t timeout_ms)
{
    if (driver == NULL || i2c_device == NULL || timeout_ms == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    driver->i2c_device = i2c_device;
    driver->timeout_ms = timeout_ms;
    esp_err_t err      = write_register(driver, PCF85063_REG_CTRL1, 0x00);
    if (err == ESP_OK)
    {
        err = normalize_unsupported_interrupt_sources(driver);
    }
    if (err != ESP_OK)
    {
        driver->i2c_device = NULL;
        driver->timeout_ms = 0;
    }
    return err;
}

esp_err_t pcf85063_driver_get_interrupt_status_copy(pcf85063_driver_t *driver, pcf85063_interrupt_status_t *out_status)
{
    if (!driver_is_valid(driver) || out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t control2   = 0;
    uint8_t timer_mode = 0;
    ESP_RETURN_ON_ERROR(read_register(driver, PCF85063_REG_CTRL2, &control2), TAG, "读取 Control_2 失败");
    ESP_RETURN_ON_ERROR(read_register(driver, PCF85063_REG_TIMER_MODE, &timer_mode), TAG, "读取 Timer_mode 失败");

    *out_status = (pcf85063_interrupt_status_t) {
        .control2_raw                  = control2,
        .timer_mode_raw                = timer_mode,
        .alarm_interrupt_enabled       = (control2 & PCF85063_CTRL2_AIE) != 0U,
        .alarm_flag                    = (control2 & PCF85063_CTRL2_AF) != 0U,
        .minute_interrupt_enabled      = (control2 & PCF85063_CTRL2_MI) != 0U,
        .half_minute_interrupt_enabled = (control2 & PCF85063_CTRL2_HMI) != 0U,
        .timer_flag                    = (control2 & PCF85063_CTRL2_TF) != 0U,
        .timer_enabled                 = (timer_mode & PCF85063_TIMER_MODE_TE) != 0U,
        .timer_interrupt_enabled       = (timer_mode & PCF85063_TIMER_MODE_TIE) != 0U,
    };
    return ESP_OK;
}

esp_err_t pcf85063_driver_get_datetime(pcf85063_driver_t *driver, pcf85063_datetime_t *out)
{
    if (!driver_is_valid(driver) || out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t reg     = PCF85063_REG_SECONDS;
    uint8_t       regs[7] = { 0 };
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(driver->i2c_device, &reg, 1, regs, sizeof(regs), driver->timeout_ms),
        TAG,
        "读取时间寄存器失败");

    const pcf85063_datetime_t value = {
        .year   = (uint16_t) (2000U + bcd_to_bin(regs[6])),
        .month  = bcd_to_bin(regs[5] & 0x1FU),
        .day    = bcd_to_bin(regs[3] & 0x3FU),
        .hour   = bcd_to_bin(regs[2] & 0x3FU),
        .minute = bcd_to_bin(regs[1] & 0x7FU),
        .second = bcd_to_bin(regs[0] & 0x7FU),
    };
    if (!datetime_is_valid(&value))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = value;
    return ESP_OK;
}

esp_err_t pcf85063_driver_get_voltage_low(pcf85063_driver_t *driver, bool *out_voltage_low)
{
    if (!driver_is_valid(driver) || out_voltage_low == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t seconds = 0;
    ESP_RETURN_ON_ERROR(read_register(driver, PCF85063_REG_SECONDS, &seconds), TAG, "读取 RTC 电压状态失败");
    *out_voltage_low = (seconds & PCF85063_SECONDS_OS) != 0;
    return ESP_OK;
}

esp_err_t pcf85063_driver_set_datetime(pcf85063_driver_t *driver, const pcf85063_datetime_t *value)
{
    if (!driver_is_valid(driver) || !datetime_is_valid(value))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t data[] = {
        PCF85063_REG_SECONDS,      bin_to_bcd(value->second),
        bin_to_bcd(value->minute), bin_to_bcd(value->hour),
        bin_to_bcd(value->day),    bin_to_bcd(calculate_weekday(value->year, value->month, value->day)),
        bin_to_bcd(value->month),  bin_to_bcd((uint8_t) (value->year - 2000U)),
    };
    return i2c_master_transmit(driver->i2c_device, data, sizeof(data), driver->timeout_ms);
}

esp_err_t pcf85063_driver_enable_alarm_interrupt(pcf85063_driver_t *driver, bool enabled)
{
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(read_register(driver, PCF85063_REG_CTRL2, &control2), TAG, "读取 Control_2 失败");
    /*
     * AF/TF 写入采用“当前值 AND 写入值”语义。配置 AIE 时对两个标志写 1，
     * 保留寄存器读取后、写回前刚好由硬件置位的新中断。
     */
    control2 |= PCF85063_CTRL2_AF | PCF85063_CTRL2_TF;
    if (enabled)
    {
        control2 |= PCF85063_CTRL2_AIE;
    }
    else
    {
        control2 &= (uint8_t) ~PCF85063_CTRL2_AIE;
    }
    return write_register(driver, PCF85063_REG_CTRL2, control2);
}

esp_err_t pcf85063_driver_get_alarm_interrupt_enabled(pcf85063_driver_t *driver, bool *out_enabled)
{
    if (!driver_is_valid(driver) || out_enabled == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(read_register(driver, PCF85063_REG_CTRL2, &control2), TAG, "读取 Control_2 失败");
    *out_enabled = (control2 & PCF85063_CTRL2_AIE) != 0U;
    return ESP_OK;
}

esp_err_t pcf85063_driver_get_alarm_flag(pcf85063_driver_t *driver, bool *pending)
{
    if (pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(read_register(driver, PCF85063_REG_CTRL2, &control2), TAG, "读取 Control_2 失败");
    *pending = (control2 & PCF85063_CTRL2_AF) != 0;
    return ESP_OK;
}

esp_err_t pcf85063_driver_clear_alarm_flag(pcf85063_driver_t *driver)
{
    uint8_t control2 = 0;
    ESP_RETURN_ON_ERROR(read_register(driver, PCF85063_REG_CTRL2, &control2), TAG, "读取 Control_2 失败");
    /* 仅清除 AF；TF 写 1，保留读改写窗口内新产生的计时器标志。 */
    control2 = (control2 & (uint8_t) ~PCF85063_CTRL2_AF) | PCF85063_CTRL2_TF;
    return write_register(driver, PCF85063_REG_CTRL2, control2);
}

esp_err_t pcf85063_driver_get_alarm(pcf85063_driver_t *driver, pcf85063_alarm_t *out_alarm)
{
    if (!driver_is_valid(driver) || out_alarm == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t reg                            = PCF85063_REG_ALARM_SECONDS;
    uint8_t       regs[PCF85063_ALARM_REG_COUNT] = { 0 };
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(driver->i2c_device, &reg, 1, regs, sizeof(regs), driver->timeout_ms),
        TAG,
        "读取告警寄存器失败");

    pcf85063_alarm_t alarm = {
        .second  = bcd_to_bin(regs[0] & 0x7FU),
        .minute  = bcd_to_bin(regs[1] & 0x7FU),
        .hour    = bcd_to_bin(regs[2] & 0x3FU),
        .day     = bcd_to_bin(regs[3] & 0x3FU),
        .weekday = bcd_to_bin(regs[4] & 0x07U),
    };
    const uint8_t field_bits[] = {
        PCF85063_ALARM_MATCH_SECOND, PCF85063_ALARM_MATCH_MINUTE,  PCF85063_ALARM_MATCH_HOUR,
        PCF85063_ALARM_MATCH_DAY,    PCF85063_ALARM_MATCH_WEEKDAY,
    };
    for (size_t index = 0; index < PCF85063_ALARM_REG_COUNT; ++index)
    {
        if ((regs[index] & PCF85063_ALARM_DISABLE) == 0U)
        {
            alarm.match_fields |= field_bits[index];
        }
    }
    if (alarm.match_fields != 0U && !alarm_is_valid(&alarm))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_alarm = alarm;
    return ESP_OK;
}

esp_err_t pcf85063_driver_set_alarm(pcf85063_driver_t *driver, const pcf85063_alarm_t *alarm)
{
    if (!driver_is_valid(driver) || !alarm_is_valid(alarm))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(pcf85063_driver_enable_alarm_interrupt(driver, false), TAG, "关闭闹钟中断失败");
    ESP_RETURN_ON_ERROR(pcf85063_driver_clear_alarm_flag(driver), TAG, "清除闹钟标志失败");

    const uint8_t data[] = {
        PCF85063_REG_ALARM_SECONDS,
        (alarm->match_fields & PCF85063_ALARM_MATCH_SECOND) != 0U ? bin_to_bcd(alarm->second) : PCF85063_ALARM_DISABLE,
        (alarm->match_fields & PCF85063_ALARM_MATCH_MINUTE) != 0U ? bin_to_bcd(alarm->minute) : PCF85063_ALARM_DISABLE,
        (alarm->match_fields & PCF85063_ALARM_MATCH_HOUR) != 0U ? bin_to_bcd(alarm->hour) : PCF85063_ALARM_DISABLE,
        (alarm->match_fields & PCF85063_ALARM_MATCH_DAY) != 0U ? bin_to_bcd(alarm->day) : PCF85063_ALARM_DISABLE,
        (alarm->match_fields & PCF85063_ALARM_MATCH_WEEKDAY) != 0U ? bin_to_bcd(alarm->weekday)
                                                                   : PCF85063_ALARM_DISABLE,
    };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(driver->i2c_device, data, sizeof(data), driver->timeout_ms),
                        TAG,
                        "写入闹钟寄存器失败");
    return pcf85063_driver_enable_alarm_interrupt(driver, true);
}
