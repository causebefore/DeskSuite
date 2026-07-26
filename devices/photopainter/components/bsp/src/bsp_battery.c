/**
 * @file bsp_battery.c
 * @brief 实现电池监测电路使能与校准 ADC 单次采样
 */
#include "bsp.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** @brief 日志标签 */
static const char *TAG = "bsp_battery";

/** @brief 每次电池电压读取的 ADC 采样数 */
#define BSP_BATTERY_SAMPLE_COUNT (16U)

/** @brief 电池 ADC 衰减配置，与官方示例的 12 dB 一致 */
#define BSP_BATTERY_ADC_ATTEN    ADC_ATTEN_DB_12

/** @brief ADC 单次采样单元句柄 */
static adc_oneshot_unit_handle_t s_adc_handle;

/** @brief ADC 曲线校准句柄 */
static adc_cali_handle_t s_cali_handle;

/** @brief GPIO1 对应的 ADC 单元与通道 */
static adc_unit_t    s_adc_unit;
static adc_channel_t s_adc_channel;

/** @brief 电池监测模块是否已完成初始化 */
static bool s_initialized;

/** @brief 按板级有效电平开启或关闭电池监测电路 */
static esp_err_t bsp_battery_set_monitor_enabled(bool enabled)
{
    const uint32_t active_level = BOARD_BATTERY_MONITOR_ENABLE_ACTIVE_HIGH ? 1U : 0U;
    return gpio_set_level(BOARD_BATTERY_MONITOR_ENABLE_GPIO,
                          enabled ? active_level : 1U - active_level);
}

/** @brief 回滚初始化期间已经取得的 ADC 与 GPIO 资源 */
static void bsp_battery_rollback_init(void)
{
    if (s_cali_handle != NULL)
    {
        (void) adc_cali_delete_scheme_curve_fitting(s_cali_handle);
        s_cali_handle = NULL;
    }
    if (s_adc_handle != NULL)
    {
        (void) adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
    }
    (void) bsp_battery_set_monitor_enabled(false);
    (void) gpio_reset_pin(BOARD_BATTERY_ADC_GPIO);
    (void) gpio_reset_pin(BOARD_BATTERY_MONITOR_ENABLE_GPIO);
}

esp_err_t bsp_battery_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t enable_config = {
        .pin_bit_mask = (1ULL << BOARD_BATTERY_MONITOR_ENABLE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&enable_config);
    if (error == ESP_OK)
    {
        error = bsp_battery_set_monitor_enabled(false);
    }
    if (error != ESP_OK)
    {
        bsp_battery_rollback_init();
        ESP_LOGE(TAG, "配置电池监测使能 GPIO 失败: %s", esp_err_to_name(error));
        return error;
    }

    error = adc_oneshot_io_to_channel(BOARD_BATTERY_ADC_GPIO, &s_adc_unit, &s_adc_channel);
    if (error != ESP_OK)
    {
        bsp_battery_rollback_init();
        ESP_LOGE(TAG, "解析电池 ADC GPIO 失败: %s", esp_err_to_name(error));
        return error;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id  = s_adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    error = adc_oneshot_new_unit(&unit_config, &s_adc_handle);
    if (error != ESP_OK)
    {
        bsp_battery_rollback_init();
        ESP_LOGE(TAG, "创建电池 ADC 单次采样单元失败: %s", esp_err_to_name(error));
        return error;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten    = BSP_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    error = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &channel_config);
    if (error != ESP_OK)
    {
        bsp_battery_rollback_init();
        ESP_LOGE(TAG, "配置电池 ADC 通道失败: %s", esp_err_to_name(error));
        return error;
    }

    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id  = s_adc_unit,
        .chan     = s_adc_channel,
        .atten    = BSP_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    error = adc_cali_create_scheme_curve_fitting(&calibration_config, &s_cali_handle);
    if (error != ESP_OK)
    {
        bsp_battery_rollback_init();
        ESP_LOGE(TAG, "创建电池 ADC 曲线校准失败: %s", esp_err_to_name(error));
        return error;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "电池监测初始化完成，采样时临时使能 GPIO%d",
             (int) BOARD_BATTERY_MONITOR_ENABLE_GPIO);
    return ESP_OK;
}

esp_err_t bsp_battery_read_voltage_mv(uint32_t *out_voltage_mv)
{
    if (out_voltage_mv == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = bsp_battery_set_monitor_enabled(true);
    if (error != ESP_OK)
    {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(BOARD_BATTERY_MONITOR_SETTLE_TIME_MS));

    uint32_t voltage_sum_mv = 0U;
    for (uint32_t index = 0U; index < BSP_BATTERY_SAMPLE_COUNT; ++index)
    {
        int sample_mv = 0;
        error         = adc_oneshot_get_calibrated_result(s_adc_handle,
                                                          s_cali_handle,
                                                          s_adc_channel,
                                                          &sample_mv);
        if (error != ESP_OK)
        {
            break;
        }
        voltage_sum_mv += (uint32_t) sample_mv;
    }

    const esp_err_t disable_error = bsp_battery_set_monitor_enabled(false);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "读取电池 ADC 失败: %s", esp_err_to_name(error));
        if (disable_error != ESP_OK)
        {
            ESP_LOGE(TAG, "ADC 失败后关闭电池监测电路也失败: %s", esp_err_to_name(disable_error));
        }
        return error;
    }
    if (disable_error != ESP_OK)
    {
        ESP_LOGE(TAG, "关闭电池监测电路失败: %s", esp_err_to_name(disable_error));
        return disable_error;
    }

    *out_voltage_mv =
        (voltage_sum_mv * BOARD_BATTERY_VOLTAGE_DIVIDER_RATIO + BSP_BATTERY_SAMPLE_COUNT / 2U)
        / BSP_BATTERY_SAMPLE_COUNT;
    return ESP_OK;
}

esp_err_t bsp_battery_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = bsp_battery_set_monitor_enabled(false);
    if (error != ESP_OK)
    {
        return error;
    }

    if (s_cali_handle != NULL)
    {
        error = adc_cali_delete_scheme_curve_fitting(s_cali_handle);
        if (error != ESP_OK)
        {
            return error;
        }
        s_cali_handle = NULL;
    }
    if (s_adc_handle != NULL)
    {
        error = adc_oneshot_del_unit(s_adc_handle);
        if (error != ESP_OK)
        {
            return error;
        }
        s_adc_handle = NULL;
    }

    (void) gpio_reset_pin(BOARD_BATTERY_ADC_GPIO);
    (void) gpio_reset_pin(BOARD_BATTERY_MONITOR_ENABLE_GPIO);
    s_initialized = false;
    return ESP_OK;
}
