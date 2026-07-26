/**
 * @file environment_service.c
 * @brief 管理按需环境联合采样、快照与生命周期
 */
#include "environment_service.h"

#include <string.h>

#include "device_battery.h"
#include "device_environment.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/** @brief 日志标签 */
static const char *TAG = "environment_service";

/** @brief 快照锁，仅保护短时间整结构复制 */
static SemaphoreHandle_t s_status_mutex;

/** @brief 采样事务锁，避免多个调用方并发访问硬件 */
static SemaphoreHandle_t s_sample_mutex;

/** @brief 最近一次联合状态 */
static environment_service_status_t s_status;

/** @brief 生命周期与连续故障日志抑制状态 */
static bool s_initialized;
static bool s_environment_error_reported;
static bool s_battery_error_reported;

/** @brief 返回当前单调运行时间，单位 ms */
static uint64_t environment_service_get_monotonic_time_ms(void)
{
    return (uint64_t) esp_timer_get_time() / 1000ULL;
}

/** @brief 对单项连续采样故障限频，并在恢复时记录一次事实 */
static void environment_service_report_sample_result(const char *name, esp_err_t error,
                                                     bool *inout_error_reported)
{
    if (error != ESP_OK)
    {
        if (!*inout_error_reported)
        {
            ESP_LOGE(TAG, "%s采样失败，将在下次按需采样时重试: %s", name, esp_err_to_name(error));
            *inout_error_reported = true;
        }
        return;
    }
    if (*inout_error_reported)
    {
        ESP_LOGI(TAG, "%s采样已恢复", name);
        *inout_error_reported = false;
    }
}

esp_err_t environment_service_sample(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sample_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    const uint64_t attempt_at_ms = environment_service_get_monotonic_time_ms();
    ESP_LOGI(TAG, "开始按需采集温湿度与电池状态");

    device_environment_measurement_t environment_measurement;
    const esp_err_t environment_error = device_environment_measure(&environment_measurement);

    device_battery_status_t battery_status;
    const esp_err_t         battery_error   = device_battery_get_status_copy(&battery_status);
    const uint64_t          completed_at_ms = environment_service_get_monotonic_time_ms();
    uint64_t                sample_count    = 0ULL;

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) != pdTRUE)
    {
        (void) xSemaphoreGive(s_sample_mutex);
        return ESP_FAIL;
    }
    s_status.last_attempt_at_ms = attempt_at_ms;
    s_status.sample_count += 1ULL;
    sample_count = s_status.sample_count;
    s_status.environment.last_error = environment_error;
    s_status.battery.last_error     = battery_error;

    if (environment_error == ESP_OK)
    {
        s_status.environment.valid            = true;
        s_status.environment.temperature_c    = environment_measurement.temperature_c;
        s_status.environment.humidity_percent = environment_measurement.humidity_percent;
        s_status.environment.updated_at_ms    = completed_at_ms;
    }
    if (battery_error == ESP_OK)
    {
        s_status.battery.valid         = true;
        s_status.battery.voltage_mv    = battery_status.voltage_mv;
        s_status.battery.percent       = battery_status.percent;
        s_status.battery.updated_at_ms = completed_at_ms;
    }
    (void) xSemaphoreGive(s_status_mutex);

    environment_service_report_sample_result("温湿度",
                                             environment_error,
                                             &s_environment_error_reported);
    environment_service_report_sample_result("电池", battery_error, &s_battery_error_reported);
    if (environment_error == ESP_OK && battery_error == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "环境联合采样完成: 第 %llu 次, 温度=%.1f ℃, 湿度=%.1f %%RH, "
                 "电量=%.1f%%, 电压=%u mV",
                 (unsigned long long) sample_count,
                 (double) environment_measurement.temperature_c,
                 (double) environment_measurement.humidity_percent,
                 (double) battery_status.percent,
                 (unsigned int) battery_status.voltage_mv);
    }
    else if (environment_error == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "温湿度采样完成: 第 %llu 次, 温度=%.1f ℃, 湿度=%.1f %%RH",
                 (unsigned long long) sample_count,
                 (double) environment_measurement.temperature_c,
                 (double) environment_measurement.humidity_percent);
    }
    else if (battery_error == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "电池采样完成: 第 %llu 次, 电量=%.1f%%, 电压=%u mV",
                 (unsigned long long) sample_count,
                 (double) battery_status.percent,
                 (unsigned int) battery_status.voltage_mv);
    }

    ESP_LOGI(TAG,
             "按需环境采样结束: 第 %llu 次, 温湿度=%s, 电池=%s, total=%llu ms",
             (unsigned long long) sample_count,
             esp_err_to_name(environment_error),
             esp_err_to_name(battery_error),
             (unsigned long long) (completed_at_ms - attempt_at_ms));
    (void) xSemaphoreGive(s_sample_mutex);
    return ESP_OK;
}

esp_err_t environment_service_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建环境状态快照互斥锁失败");
        return ESP_ERR_NO_MEM;
    }
    s_sample_mutex = xSemaphoreCreateMutex();
    if (s_sample_mutex == NULL)
    {
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = NULL;
        ESP_LOGE(TAG, "创建环境采样事务互斥锁失败");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.environment.last_error = ESP_ERR_NOT_FOUND;
    s_status.battery.last_error     = ESP_ERR_NOT_FOUND;
    s_initialized                   = true;
    s_environment_error_reported    = false;
    s_battery_error_reported        = false;
    ESP_LOGI(TAG, "环境联合采样 Service 初始化完成");
    return ESP_OK;
}

esp_err_t environment_service_get_status_copy(environment_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_status_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    *out_status = s_status;
    (void) xSemaphoreGive(s_status_mutex);
    return ESP_OK;
}

esp_err_t environment_service_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    vSemaphoreDelete(s_sample_mutex);
    vSemaphoreDelete(s_status_mutex);
    s_sample_mutex               = NULL;
    s_status_mutex               = NULL;
    s_initialized                = false;
    s_environment_error_reported = false;
    s_battery_error_reported     = false;
    memset(&s_status, 0, sizeof(s_status));
    return ESP_OK;
}
