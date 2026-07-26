/**
 * @file bsp_rtc.c
 * @brief 在板载共享 I2C 总线上装配 PCF8563 RTC
 */
#include "bsp.h"

#include "board.h"
#include "bsp_i2c_internal.h"
#include "esp_log.h"
#include "pcf8563.h"

/** @brief 日志标签 */
static const char *TAG = "bsp_rtc";

/** @brief BSP 唯一持有的 PCF8563 驱动实例 */
static pcf8563_t s_rtc;
/** @brief 板载 RTC 生命周期状态 */
typedef enum
{
    BSP_RTC_STATE_UNINITIALIZED = 0,
    BSP_RTC_STATE_INITIALIZED,
    BSP_RTC_STATE_BUS_RELEASE_PENDING,
} bsp_rtc_state_t;

/** @brief 当前板载 RTC 生命周期状态 */
static bsp_rtc_state_t s_state;

esp_err_t bsp_rtc_init(void)
{
    if (s_state != BSP_RTC_STATE_UNINITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_handle_t bus;
    esp_err_t error = bsp_i2c_acquire(&bus);
    if (error != ESP_OK)
    {
        return error;
    }
    error = pcf8563_init(&s_rtc, bus, BOARD_PCF8563_ADDRESS, BOARD_I2C_SCL_SPEED_HZ);
    if (error != ESP_OK)
    {
        (void) bsp_i2c_release();
        ESP_LOGE(TAG, "板载 PCF8563 初始化失败: %s", esp_err_to_name(error));
        return error;
    }

    s_state = BSP_RTC_STATE_INITIALIZED;
    ESP_LOGI(TAG, "板载 PCF8563 初始化完成，地址 0x%02X", BOARD_PCF8563_ADDRESS);
    return ESP_OK;
}

esp_err_t bsp_rtc_get_snapshot_copy(bsp_rtc_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != BSP_RTC_STATE_INITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    pcf8563_snapshot_t snapshot;
    const esp_err_t error = pcf8563_get_snapshot_copy(&s_rtc, &snapshot);
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

esp_err_t bsp_rtc_get_voltage_low(bool *out_voltage_low)
{
    if (out_voltage_low == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != BSP_RTC_STATE_INITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return pcf8563_get_voltage_low(&s_rtc, out_voltage_low);
}

esp_err_t bsp_rtc_set_datetime(const bsp_rtc_datetime_t *datetime)
{
    if (datetime == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != BSP_RTC_STATE_INITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const pcf8563_datetime_t driver_datetime = {
        .year   = datetime->year,
        .month  = datetime->month,
        .day    = datetime->day,
        .hour   = datetime->hour,
        .minute = datetime->minute,
        .second = datetime->second,
    };
    return pcf8563_set_datetime(&s_rtc, &driver_datetime);
}

esp_err_t bsp_rtc_deinit(void)
{
    if (s_state == BSP_RTC_STATE_UNINITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == BSP_RTC_STATE_INITIALIZED)
    {
        const esp_err_t driver_error = pcf8563_deinit(&s_rtc);
        if (driver_error != ESP_OK)
        {
            return driver_error;
        }
        s_state = BSP_RTC_STATE_BUS_RELEASE_PENDING;
    }

    const esp_err_t error = bsp_i2c_release();
    if (error != ESP_OK)
    {
        return error;
    }
    s_state = BSP_RTC_STATE_UNINITIALIZED;
    return ESP_OK;
}
