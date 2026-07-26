/**
 * @file device_status_upload_app.c
 * @brief 编排按需环境采样到设备状态上传协议的数据流
 */
#include "device_status_upload_app.h"

#include "device_status_protocol.h"
#include "environment_service.h"
#include "esp_check.h"
#include "esp_log.h"

/** @brief 日志标签 */
static const char *TAG = "status_upload_app";

esp_err_t device_status_upload_app_upload(const device_status_upload_app_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->base_url != NULL && config->base_url[0] != '\0'
                            && config->timeout_ms > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "设备状态上传配置无效");

    ESP_RETURN_ON_ERROR(environment_service_sample(), TAG, "执行上传前按需环境采样失败");

    environment_service_status_t snapshot;
    ESP_RETURN_ON_ERROR(environment_service_get_status_copy(&snapshot),
                        TAG,
                        "读取环境联合快照失败");
    if (!snapshot.battery.valid)
    {
        ESP_LOGW(TAG, "尚无有效电池状态，跳过上传");
        return snapshot.battery.last_error != ESP_OK ? snapshot.battery.last_error
                                                     : ESP_ERR_INVALID_STATE;
    }
    if (snapshot.battery.last_error != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "最近一次电池采样失败，跳过上传: %s",
                 esp_err_to_name(snapshot.battery.last_error));
        return snapshot.battery.last_error;
    }

    const bool has_environment =
        snapshot.environment.valid && snapshot.environment.last_error == ESP_OK;
    if (!has_environment)
    {
        ESP_LOGW(TAG, "最近一次温湿度采样无效，本轮只上传电池状态");
    }
    const device_status_protocol_upload_t upload = {
        .has_environment    = has_environment,
        .temperature_c      = snapshot.environment.temperature_c,
        .humidity_percent   = snapshot.environment.humidity_percent,
        .battery_percent    = snapshot.battery.percent,
        .battery_voltage_mv = snapshot.battery.voltage_mv,
    };
    if (has_environment)
    {
        ESP_LOGI(TAG,
                 "开始上传设备状态: 温度=%.1f ℃, 湿度=%.1f %%RH, 电量=%.1f%%, 电压=%u mV",
                 (double) upload.temperature_c,
                 (double) upload.humidity_percent,
                 (double) upload.battery_percent,
                 (unsigned int) upload.battery_voltage_mv);
    }
    else
    {
        ESP_LOGI(TAG,
                 "开始上传设备状态: 本轮无有效温湿度, 电量=%.1f%%, 电压=%u mV",
                 (double) upload.battery_percent,
                 (unsigned int) upload.battery_voltage_mv);
    }

    const esp_err_t error = device_status_protocol_upload_borrow(config->base_url,
                                                                 config->token,
                                                                 config->device_id,
                                                                 &upload,
                                                                 config->timeout_ms);
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "设备状态上传完成");
    }
    return error;
}
