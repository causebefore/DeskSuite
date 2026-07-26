/*
 * 文件职责：串行化同步块设备事务，并把板级 SD 卡收敛为 Device 存储能力。
 */
#include "device_storage.h"

#include <stdbool.h>

#include "bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "device_storage";

static bool              s_initialized;
static SemaphoreHandle_t s_transaction_lock;

/**
 * @brief 在设备事务锁内同步执行无参数 BSP 操作
 *
 * 调用者必须位于允许阻塞的 Task 上下文；本函数会无限等待事务锁，并保证底层操作返回后再
 * 释放锁。operation 是仅在持锁期间调用的内部函数指针，不会被保存。
 *
 * @param[in] operation 要串行执行的 BSP 操作
 * @return ESP_OK 操作成功；ESP_ERR_INVALID_STATE 组件尚未初始化；或 operation 返回的错误码
 */
static esp_err_t device_storage_execute_locked(esp_err_t (*operation)(void))
{
    ESP_RETURN_ON_FALSE(s_initialized && s_transaction_lock != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "外部存储设备尚未初始化");
    xSemaphoreTake(s_transaction_lock, portMAX_DELAY);
    const esp_err_t error = operation();
    xSemaphoreGive(s_transaction_lock);
    return error;
}

esp_err_t device_storage_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_transaction_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_transaction_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建外部存储事务锁失败");

    const esp_err_t error = bsp_storage_init();
    if (error != ESP_OK)
    {
        vSemaphoreDelete(s_transaction_lock);
        s_transaction_lock = NULL;
        return error;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t device_storage_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_initialized && s_transaction_lock != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "外部存储设备尚未初始化");

    xSemaphoreTake(s_transaction_lock, portMAX_DELAY);
    const esp_err_t error = bsp_storage_deinit();
    if (error == ESP_OK)
    {
        s_initialized = false;
    }
    xSemaphoreGive(s_transaction_lock);
    if (error != ESP_OK)
    {
        return error;
    }

    vSemaphoreDelete(s_transaction_lock);
    s_transaction_lock = NULL;
    return ESP_OK;
}

esp_err_t device_storage_check_ready(void)
{
    return device_storage_execute_locked(bsp_storage_check_ready);
}

esp_err_t device_storage_get_info_copy(device_storage_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(out_info != NULL, ESP_ERR_INVALID_ARG, TAG, "外部存储信息输出为空");
    ESP_RETURN_ON_FALSE(s_initialized && s_transaction_lock != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "外部存储设备尚未初始化");

    xSemaphoreTake(s_transaction_lock, portMAX_DELAY);
    bsp_storage_info_t bsp_info;
    const esp_err_t    error = bsp_storage_get_info_copy(&bsp_info);
    if (error == ESP_OK)
    {
        out_info->sector_count      = bsp_info.sector_count;
        out_info->sector_size_bytes = bsp_info.sector_size_bytes;
        out_info->capacity_bytes    = bsp_info.capacity_bytes;
    }
    xSemaphoreGive(s_transaction_lock);
    return error;
}

esp_err_t device_storage_read_sectors(uint32_t start_sector, size_t sector_count, void *out_data)
{
    ESP_RETURN_ON_FALSE(s_initialized && s_transaction_lock != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "外部存储设备尚未初始化");
    xSemaphoreTake(s_transaction_lock, portMAX_DELAY);
    const esp_err_t error = bsp_storage_read_sectors(start_sector, sector_count, out_data);
    xSemaphoreGive(s_transaction_lock);
    return error;
}

esp_err_t device_storage_write_sectors(uint32_t start_sector, size_t sector_count, const void *data)
{
    ESP_RETURN_ON_FALSE(s_initialized && s_transaction_lock != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "外部存储设备尚未初始化");
    xSemaphoreTake(s_transaction_lock, portMAX_DELAY);
    const esp_err_t error = bsp_storage_write_sectors(start_sector, sector_count, data);
    xSemaphoreGive(s_transaction_lock);
    return error;
}
