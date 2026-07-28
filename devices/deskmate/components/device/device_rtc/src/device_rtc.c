/**
 * @file device_rtc.c
 * @brief 把板载 PCF85063 日历时钟收敛为稳定的 Device API
 */
#include "device_rtc.h"

#include "bsp.h"

static bool s_initialized;

static uint8_t calculate_weekday(const device_rtc_datetime_t *datetime)
{
    static const uint8_t month_offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int                  year            = datetime->year;
    if (datetime->month < 3)
    {
        year--;
    }
    return (uint8_t) ((year + year / 4 - year / 100 + year / 400 + month_offsets[datetime->month - 1] + datetime->day)
                      % 7);
}

esp_err_t device_rtc_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }
    const esp_err_t error = bsp_rtc_init();
    if (error == ESP_OK)
    {
        s_initialized = true;
    }
    return error;
}

esp_err_t device_rtc_read_snapshot(device_rtc_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    bsp_rtc_datetime_t datetime;
    const esp_err_t    error = bsp_rtc_read_datetime(&datetime);
    if (error != ESP_OK)
    {
        return error;
    }
    out_snapshot->datetime.year   = datetime.year;
    out_snapshot->datetime.month  = datetime.month;
    out_snapshot->datetime.day    = datetime.day;
    out_snapshot->datetime.hour   = datetime.hour;
    out_snapshot->datetime.minute = datetime.minute;
    out_snapshot->datetime.second = datetime.second;
    out_snapshot->weekday         = calculate_weekday(&out_snapshot->datetime);
    return bsp_rtc_read_voltage_low(&out_snapshot->voltage_low);
}

esp_err_t device_rtc_read_voltage_low(bool *out_voltage_low)
{
    if (out_voltage_low == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_rtc_read_voltage_low(out_voltage_low);
}

esp_err_t device_rtc_set_datetime(const device_rtc_datetime_t *datetime)
{
    if (datetime == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bsp_rtc_datetime_t bsp_datetime = {
        .year   = datetime->year,
        .month  = datetime->month,
        .day    = datetime->day,
        .hour   = datetime->hour,
        .minute = datetime->minute,
        .second = datetime->second,
    };
    return bsp_rtc_set_datetime(&bsp_datetime);
}

esp_err_t device_rtc_set_alarm(const device_rtc_alarm_t *alarm)
{
    if (alarm == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bsp_rtc_alarm_t bsp_alarm = {
        .second       = alarm->second,
        .minute       = alarm->minute,
        .hour         = alarm->hour,
        .day          = alarm->day,
        .weekday      = alarm->weekday,
        .match_fields = alarm->match_fields,
    };
    return bsp_rtc_set_alarm(&bsp_alarm);
}

esp_err_t device_rtc_read_alarm(device_rtc_alarm_t *out_alarm)
{
    if (out_alarm == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    bsp_rtc_alarm_t bsp_alarm;
    const esp_err_t error = bsp_rtc_read_alarm(&bsp_alarm);
    if (error == ESP_OK)
    {
        *out_alarm = (device_rtc_alarm_t) {
            .second       = bsp_alarm.second,
            .minute       = bsp_alarm.minute,
            .hour         = bsp_alarm.hour,
            .day          = bsp_alarm.day,
            .weekday      = bsp_alarm.weekday,
            .match_fields = bsp_alarm.match_fields,
        };
    }
    return error;
}

esp_err_t device_rtc_enable_alarm_interrupt(bool enabled)
{
    return s_initialized ? bsp_rtc_enable_alarm_interrupt(enabled) : ESP_ERR_INVALID_STATE;
}

esp_err_t device_rtc_read_alarm_interrupt_enabled(bool *out_enabled)
{
    if (out_enabled == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return s_initialized ? bsp_rtc_read_alarm_interrupt_enabled(out_enabled) : ESP_ERR_INVALID_STATE;
}

esp_err_t device_rtc_read_alarm_flag(bool *out_pending)
{
    if (out_pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return s_initialized ? bsp_rtc_read_alarm_flag(out_pending) : ESP_ERR_INVALID_STATE;
}

esp_err_t device_rtc_clear_alarm_flag(void)
{
    return s_initialized ? bsp_rtc_clear_alarm_flag() : ESP_ERR_INVALID_STATE;
}

esp_err_t device_rtc_read_interrupt_asserted(bool *out_asserted)
{
    if (out_asserted == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return s_initialized ? bsp_rtc_read_interrupt_asserted(out_asserted) : ESP_ERR_INVALID_STATE;
}

esp_err_t device_rtc_set_interrupt_callback_borrow(device_rtc_interrupt_callback_t callback, void *context)
{
    return s_initialized ? bsp_rtc_set_interrupt_callback_borrow(callback, context) : ESP_ERR_INVALID_STATE;
}

esp_err_t device_rtc_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = bsp_rtc_deinit();
    if (error == ESP_OK)
    {
        s_initialized = false;
    }
    return error;
}
