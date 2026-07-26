/*
 * 文件职责：在调用者上下文同步采样电池，并维护线程安全快照。
 */
#include "device_battery.h"

#include "bsp.h"
#include "device_battery_logic.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct
{
    device_battery_snapshot_t snapshot;
    uint16_t                  filtered_mv;
    bool                      initialized;
} device_battery_context_t;

static const char *TAG = "device_battery";

static device_battery_context_t s_context;
static SemaphoreHandle_t        s_lock;

#define DEVICE_BATTERY_EMA_ALPHA_PERCENT 20U
#define DEVICE_BATTERY_LOW_MV            3450U

/**
 * @brief 初始化同步电池采样能力
 */
esp_err_t device_battery_init(void)
{
    if (s_context.initialized)
    {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建电池快照锁失败");
    const esp_err_t error = bsp_battery_init();
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
 * @brief 在调用者上下文同步执行一次电池采样
 */
esp_err_t device_battery_sample(void)
{
    ESP_RETURN_ON_FALSE(s_context.initialized, ESP_ERR_INVALID_STATE, TAG, "电池能力未初始化");

    bsp_battery_sample_t sample = { 0 };
    ESP_RETURN_ON_ERROR(bsp_battery_read_sample(&sample), TAG, "读取电池采样失败");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_context.filtered_mv =
        device_battery_ema_mv(s_context.filtered_mv, sample.battery_mv, DEVICE_BATTERY_EMA_ALPHA_PERCENT);
    s_context.snapshot.voltage_mv = s_context.filtered_mv;
    s_context.snapshot.percent    = device_battery_percent_from_mv(s_context.filtered_mv);
    s_context.snapshot.low        = device_battery_is_low(s_context.filtered_mv, DEVICE_BATTERY_LOW_MV);
    s_context.snapshot.valid      = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/**
 * @brief 获取最近一次电池快照
 */
esp_err_t device_battery_get_snapshot_copy(device_battery_snapshot_t *out_snapshot)
{
    ESP_RETURN_ON_FALSE(out_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "电池快照输出为空");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "电池能力未初始化");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_snapshot = s_context.snapshot;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t device_battery_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_context.initialized, ESP_ERR_INVALID_STATE, TAG, "电池能力未初始化");
    ESP_RETURN_ON_ERROR(bsp_battery_deinit(), TAG, "释放电池 BSP 失败");
    vSemaphoreDelete(s_lock);
    s_lock    = NULL;
    s_context = (device_battery_context_t) { 0 };
    return ESP_OK;
}
