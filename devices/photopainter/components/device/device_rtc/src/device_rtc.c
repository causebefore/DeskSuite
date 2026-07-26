/**
 * @file device_rtc.c
 * @brief 把板载日历时钟收敛为稳定的 Device API
 */
#include "device_rtc.h"

#include "bsp.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "device_rtc";

/** @brief Device RTC 能力生命周期状态 */
static bool s_initialized;

esp_err_t device_rtc_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = bsp_rtc_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "BSP RTC 初始化失败: %s", esp_err_to_name(error));
        return error;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "设备 RTC 能力初始化完成");
    return ESP_OK;
}

esp_err_t device_rtc_get_snapshot_copy(device_rtc_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bsp_rtc_snapshot_t snapshot;
    const esp_err_t error = bsp_rtc_get_snapshot_copy(&snapshot);
    if (error != ESP_OK)
    {
        return error;
    }
    out_snapshot->datetime.year   = snapshot.datetime.year;
    out_snapshot->datetime.month  = snapshot.datetime.month;
    out_snapshot->datetime.day    = snapshot.datetime.day;
    out_snapshot->datetime.hour   = snapshot.datetime.hour;
    out_snapshot->datetime.minute = snapshot.datetime.minute;
    out_snapshot->datetime.second = snapshot.datetime.second;
    out_snapshot->weekday         = snapshot.weekday;
    out_snapshot->voltage_low     = snapshot.voltage_low;
    return ESP_OK;
}

esp_err_t device_rtc_get_voltage_low(bool *out_voltage_low)
{
    if (out_voltage_low == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_rtc_get_voltage_low(out_voltage_low);
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

esp_err_t device_rtc_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = bsp_rtc_deinit();
    if (error != ESP_OK)
    {
        return error;
    }
    s_initialized = false;
    return ESP_OK;
}
