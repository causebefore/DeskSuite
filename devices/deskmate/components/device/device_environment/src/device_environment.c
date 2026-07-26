/*
 * 文件职责：在调用者上下文同步采样温湿度，并维护线程安全快照。
 */
#include "device_environment.h"

#include "bsp.h"
#include "device_environment_logic.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct
{
    device_environment_snapshot_t snapshot;
    bool                          initialized;
    bool                          have_filtered;
    int16_t                       filtered_temperature_centi;
    int16_t                       filtered_humidity_centi;
} device_environment_context_t;

static const char *TAG = "device_environment";

static device_environment_context_t s_context;
static SemaphoreHandle_t            s_lock;

#define DEVICE_ENVIRONMENT_EMA_ALPHA_PERCENT 20U

/**
 * @brief 初始化同步环境传感器能力
 */
esp_err_t device_environment_init(void)
{
    if (s_context.initialized)
    {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建传感器快照锁失败");
    const esp_err_t error = bsp_environment_init();
    if (error != ESP_OK)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return error;
    }
    s_context.initialized = true;
    return ESP_OK;
}

/**
 * @brief 在调用者上下文同步执行一次温湿度采样
 */
esp_err_t device_environment_sample(void)
{
    ESP_RETURN_ON_FALSE(s_context.initialized, ESP_ERR_INVALID_STATE, TAG, "传感器能力未初始化");

    bsp_environment_sample_t sample = { 0 };
    const esp_err_t          err    = bsp_environment_read_sample(&sample);
    if (err != ESP_OK)
    {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_context.have_filtered)
    {
        s_context.filtered_temperature_centi = sample.temperature_centi;
        s_context.filtered_humidity_centi    = (int16_t) sample.humidity_centi;
        s_context.have_filtered              = true;
    }
    else
    {
        s_context.filtered_temperature_centi = device_environment_ema_centi(s_context.filtered_temperature_centi,
                                                                            sample.temperature_centi,
                                                                            DEVICE_ENVIRONMENT_EMA_ALPHA_PERCENT);
        s_context.filtered_humidity_centi    = device_environment_ema_centi(s_context.filtered_humidity_centi,
                                                                            (int16_t) sample.humidity_centi,
                                                                            DEVICE_ENVIRONMENT_EMA_ALPHA_PERCENT);
    }
    s_context.snapshot.temperature_centi = s_context.filtered_temperature_centi;
    s_context.snapshot.humidity_centi    = (uint16_t) s_context.filtered_humidity_centi;
    s_context.snapshot.valid             = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/**
 * @brief 获取最近一次环境传感器快照
 */
esp_err_t device_environment_get_snapshot_copy(device_environment_snapshot_t *out_snapshot)
{
    ESP_RETURN_ON_FALSE(out_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "传感器快照输出为空");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "传感器能力未初始化");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_snapshot = s_context.snapshot;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t device_environment_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_context.initialized, ESP_ERR_INVALID_STATE, TAG, "传感器能力未初始化");
    ESP_RETURN_ON_ERROR(bsp_environment_deinit(), TAG, "释放环境传感器 BSP 失败");
    vSemaphoreDelete(s_lock);
    s_lock    = NULL;
    s_context = (device_environment_context_t) { 0 };
    return ESP_OK;
}
