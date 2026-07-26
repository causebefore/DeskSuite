/*
 * 文件职责：封装电池 ADC 或电量芯片的原始数据读取能力。
 * 主要依赖：board_power.h、ESP-IDF ADC 相关驱动。
 * 调用方：battery。
 */
#include "bsp.h"

#include <stdbool.h>

#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "bsp_battery";

static adc_oneshot_unit_handle_t s_adc_unit;
static adc_cali_handle_t         s_adc_cali;
static bool                      s_cali_ready;

static bool create_adc_calibration(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BOARD_BATTERY_ADC_UNIT,
        .chan     = BOARD_BATTERY_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    const adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = BOARD_BATTERY_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali) == ESP_OK;
#else
    return false;
#endif
}

esp_err_t bsp_battery_init(void)
{
    if (s_adc_unit != NULL)
    {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = BOARD_BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_unit), TAG, "创建 ADC unit 失败");

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_unit, BOARD_BATTERY_ADC_CHANNEL, &chan_cfg),
                        TAG,
                        "配置 ADC 通道失败");

    s_cali_ready = create_adc_calibration();
    ESP_LOGI(TAG,
             "电池 ADC 初始化完成: gpio=%d channel=%d cali=%d",
             BOARD_BATTERY_ADC_GPIO,
             BOARD_BATTERY_ADC_CHANNEL,
             s_cali_ready);
    return ESP_OK;
}

esp_err_t bsp_battery_read_sample(bsp_battery_sample_t *sample)
{
    if (sample == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(bsp_battery_init(), TAG, "初始化电池 ADC 失败");

    int raw = 0;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_unit, BOARD_BATTERY_ADC_CHANNEL, &raw), TAG, "读取电池 ADC 失败");

    int pin_mv = 0;
    if (s_cali_ready)
    {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali, raw, &pin_mv), TAG, "ADC 校准失败");
    }
    else
    {
        /* ADC_ATTEN_DB_12 在 ESP32-S3 上实际满量程约 2450mV（非线性），4095 ≠ 3300mV。
         * 无 eFuse 校准时用 2450 近似，避免经分压后电量系统性偏高。 */
        pin_mv = (raw * 2450) / 4095;
    }

    sample->raw        = (uint16_t) raw;
    sample->pin_mv     = (uint16_t) pin_mv;
    sample->battery_mv = (uint16_t) ((((uint32_t) sample->pin_mv * BOARD_BATTERY_DIVIDER_NUMERATOR)
                                      + (BOARD_BATTERY_DIVIDER_DENOMINATOR / 2U))
                                     / BOARD_BATTERY_DIVIDER_DENOMINATOR);
    return ESP_OK;
}

esp_err_t bsp_battery_deinit(void)
{
    /* s_adc_unit 兼作已初始化标志：未初始化直接返回（幂等） */
    if (s_adc_unit == NULL)
    {
        return ESP_OK;
    }

    /* 释放 ADC 校准句柄：按创建时使用的 scheme 对应删除（与 create_adc_calibration
     * 的编译期分支一致） */
    if (s_adc_cali != NULL)
    {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        (void) adc_cali_delete_scheme_curve_fitting(s_adc_cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        (void) adc_cali_delete_scheme_line_fitting(s_adc_cali);
#endif
        s_adc_cali   = NULL;
        s_cali_ready = false;
    }

    /* 释放 ADC oneshot unit */
    (void) adc_oneshot_del_unit(s_adc_unit);
    s_adc_unit = NULL;

    ESP_LOGI(TAG, "电池 ADC 反初始化完成");
    return ESP_OK;
}
