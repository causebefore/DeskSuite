/**
 * @file environment_service.c
 * @brief 管理环境与电池按需采样、联合快照和更新通知
 */
#include "environment_service.h"

#include <stdbool.h>
#include <string.h>

#include "device_battery.h"
#include "device_environment.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "environment_service";

ESP_EVENT_DEFINE_BASE(ENVIRONMENT_SERVICE_EVENT);

static SemaphoreHandle_t              s_snapshot_mutex;
static SemaphoreHandle_t              s_sample_mutex;
static environment_service_snapshot_t s_snapshot;
static bool                           s_initialized;
static bool                           s_environment_error_reported;
static bool                           s_battery_error_reported;

static uint64_t monotonic_time_ms(void)
{
    return (uint64_t) esp_timer_get_time() / 1000ULL;
}

static void report_sample_result(const char *name, esp_err_t error, bool *error_reported)
{
    if (error != ESP_OK)
    {
        if (!*error_reported)
        {
            ESP_LOGE(TAG, "%s采样失败，将在下一次请求时重试: %s", name, esp_err_to_name(error));
            *error_reported = true;
        }
        return;
    }
    if (*error_reported)
    {
        ESP_LOGI(TAG, "%s采样已恢复", name);
        *error_reported = false;
    }
}

static void post_update_event(environment_service_event_t event)
{
    const esp_err_t error = esp_event_post(ENVIRONMENT_SERVICE_EVENT, event, NULL, 0, 0);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "投递环境 Service 更新事件失败: event=%d err=%s", (int) event, esp_err_to_name(error));
    }
}

/**
 * @brief 串行执行选中的硬件采样，并在一次短锁内提交快照
 */
static esp_err_t sample_selected(bool sample_environment, bool sample_battery)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!sample_environment && !sample_battery)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_sample_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }

    const uint64_t                attempt_at_ms        = monotonic_time_ms();
    device_environment_snapshot_t environment_snapshot = { 0 };
    device_battery_snapshot_t     battery_snapshot     = { 0 };
    esp_err_t                     environment_error    = ESP_ERR_NOT_FOUND;
    esp_err_t                     battery_error        = ESP_ERR_NOT_FOUND;

    if (sample_environment)
    {
        environment_error = device_environment_sample();
        if (environment_error == ESP_OK)
        {
            environment_error = device_environment_get_snapshot_copy(&environment_snapshot);
        }
    }
    if (sample_battery)
    {
        battery_error = device_battery_sample();
        if (battery_error == ESP_OK)
        {
            battery_error = device_battery_get_snapshot_copy(&battery_snapshot);
        }
    }

    const uint64_t completed_at_ms = monotonic_time_ms();
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE)
    {
        (void) xSemaphoreGive(s_sample_mutex);
        return ESP_FAIL;
    }

    s_snapshot.last_attempt_at_ms = attempt_at_ms;
    s_snapshot.sample_count += 1ULL;
    if (sample_environment)
    {
        s_snapshot.environment.last_error = environment_error;
        if (environment_error == ESP_OK)
        {
            s_snapshot.environment.temperature_centi = environment_snapshot.temperature_centi;
            s_snapshot.environment.humidity_centi    = environment_snapshot.humidity_centi;
            s_snapshot.environment.valid             = environment_snapshot.valid;
            s_snapshot.environment.updated_at_ms     = completed_at_ms;
        }
    }
    if (sample_battery)
    {
        s_snapshot.battery.last_error = battery_error;
        if (battery_error == ESP_OK)
        {
            s_snapshot.battery.voltage_mv    = battery_snapshot.voltage_mv;
            s_snapshot.battery.percent       = battery_snapshot.percent;
            s_snapshot.battery.low           = battery_snapshot.low;
            s_snapshot.battery.valid         = battery_snapshot.valid;
            s_snapshot.battery.updated_at_ms = completed_at_ms;
        }
    }
    (void) xSemaphoreGive(s_snapshot_mutex);

    if (sample_environment)
    {
        report_sample_result("温湿度", environment_error, &s_environment_error_reported);
    }
    if (sample_battery)
    {
        report_sample_result("电池", battery_error, &s_battery_error_reported);
    }
    (void) xSemaphoreGive(s_sample_mutex);

    if (sample_environment)
    {
        post_update_event(ENVIRONMENT_SERVICE_EVENT_ENVIRONMENT_UPDATED);
    }
    if (sample_battery)
    {
        post_update_event(ENVIRONMENT_SERVICE_EVENT_BATTERY_UPDATED);
    }
    return ESP_OK;
}

esp_err_t environment_service_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_sample_mutex = xSemaphoreCreateMutex();
    if (s_sample_mutex == NULL)
    {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.environment.last_error = ESP_ERR_NOT_FOUND;
    s_snapshot.battery.last_error     = ESP_ERR_NOT_FOUND;
    s_initialized                     = true;
    s_environment_error_reported      = false;
    s_battery_error_reported          = false;
    ESP_LOGI(TAG, "环境联合采样 Service 初始化完成");
    return ESP_OK;
}

esp_err_t environment_service_sample_environment(void)
{
    return sample_selected(true, false);
}

esp_err_t environment_service_sample_battery(void)
{
    return sample_selected(false, true);
}

esp_err_t environment_service_get_snapshot_copy(environment_service_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    *out_snapshot = s_snapshot;
    (void) xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}

esp_err_t environment_service_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    vSemaphoreDelete(s_sample_mutex);
    vSemaphoreDelete(s_snapshot_mutex);
    s_sample_mutex               = NULL;
    s_snapshot_mutex             = NULL;
    s_initialized                = false;
    s_environment_error_reported = false;
    s_battery_error_reported     = false;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    return ESP_OK;
}
