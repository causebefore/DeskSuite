/**
 * @file device_battery.c
 * @brief 实现设备电池电压采样与电量曲线换算
 */
#include "device_battery.h"

#include <stddef.h>

#include "bsp.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "device_battery";

/** @brief 官方 ESPHome 示例中的电池电压—电量校准点 */
typedef struct
{
    uint32_t voltage_mv;
    float    percent;
} device_battery_curve_point_t;

/** @brief 按电压升序保存的电池电量校准曲线 */
static const device_battery_curve_point_t s_battery_curve[] = {
    { 3270U, 0.0F   },
    { 3300U, 5.0F   },
    { 3410U, 10.0F  },
    { 3490U, 20.0F  },
    { 3580U, 30.0F  },
    { 3680U, 40.0F  },
    { 3750U, 50.0F  },
    { 3800U, 60.0F  },
    { 3850U, 70.0F  },
    { 3910U, 80.0F  },
    { 3960U, 90.0F  },
    { 4150U, 100.0F },
};

/** @brief 电池监测能力是否已初始化 */
static bool s_initialized;

/**
 * @brief 在相邻官方校准点之间线性插值，并把结果限制在 0～100%
 *
 * @param[in] voltage_mv 电池电压，单位 mV
 * @return 换算后的电量百分比
 */
static float device_battery_voltage_to_percent(uint32_t voltage_mv)
{
    const size_t point_count = sizeof(s_battery_curve) / sizeof(s_battery_curve[0]);
    if (voltage_mv <= s_battery_curve[0].voltage_mv)
    {
        return s_battery_curve[0].percent;
    }

    for (size_t index = 1U; index < point_count; ++index)
    {
        if (voltage_mv <= s_battery_curve[index].voltage_mv)
        {
            const device_battery_curve_point_t *lower = &s_battery_curve[index - 1U];
            const device_battery_curve_point_t *upper = &s_battery_curve[index];
            const float position = (float) (voltage_mv - lower->voltage_mv)
                                   / (float) (upper->voltage_mv - lower->voltage_mv);
            return lower->percent + position * (upper->percent - lower->percent);
        }
    }
    return s_battery_curve[point_count - 1U].percent;
}

esp_err_t device_battery_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = bsp_battery_init();
    if (error != ESP_OK)
    {
        return error;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "设备电池监测能力初始化完成");
    return ESP_OK;
}

esp_err_t device_battery_get_status_copy(device_battery_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t        voltage_mv = 0U;
    const esp_err_t error      = bsp_battery_read_voltage_mv(&voltage_mv);
    if (error != ESP_OK)
    {
        return error;
    }

    out_status->voltage_mv = voltage_mv;
    out_status->percent    = device_battery_voltage_to_percent(voltage_mv);
    return ESP_OK;
}

esp_err_t device_battery_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = bsp_battery_deinit();
    if (error == ESP_OK)
    {
        s_initialized = false;
    }
    return error;
}
