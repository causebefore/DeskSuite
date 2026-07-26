/**
 * @file sd_card_service.c
 * @brief 编排 SD 卡插拔事实与自动挂载事务
 */
#include "sd_card_service.h"

#include "device_sd.h"
#include "esp_log.h"
#include "sd_card_service_internal.h"

/** @brief 日志标签 */
static const char *TAG = "sd_card_service";

/** @brief Service 生命周期状态 */
static bool s_running;
static bool s_cleanup_failed;

/** @brief 上一次已报告的物理插卡状态 */
static bool s_last_present;
static bool s_last_present_valid;

esp_err_t sd_card_service_reconcile_card(void)
{
    device_sd_status_t status;
    esp_err_t          error = device_sd_get_status_copy(&status);
    if (error != ESP_OK)
    {
        return error;
    }

    if (!s_last_present_valid || status.card_present != s_last_present)
    {
        ESP_LOGI(TAG, "%s", status.card_present ? "检测到 SD 卡插入" : "检测到 SD 卡拔出");
        s_last_present       = status.card_present;
        s_last_present_valid = true;
    }

    if (status.card_present && !status.mounted)
    {
        error = device_sd_mount();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "插卡后自动挂载失败: %s", esp_err_to_name(error));
        }
        else
        {
            ESP_LOGI(TAG, "SD 卡文件系统已挂载，照片存储可用");
        }
        return error;
    }
    if (!status.card_present && status.mounted)
    {
        error = device_sd_unmount();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "拔卡后自动卸载失败: %s", esp_err_to_name(error));
        }
        else
        {
            ESP_LOGI(TAG, "SD 卡文件系统已卸载");
        }
        return error;
    }
    return ESP_OK;
}

esp_err_t sd_card_service_start(void)
{
    if (s_running || s_cleanup_failed)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t reconcile_error = sd_card_service_reconcile_card();
    if (reconcile_error == ESP_ERR_INVALID_STATE)
    {
        return reconcile_error;
    }

    const esp_err_t error = sd_card_service_task_start();
    if (error != ESP_OK)
    {
        if (error == ESP_ERR_TIMEOUT)
        {
            s_cleanup_failed = true;
        }
        device_sd_status_t status;
        if (device_sd_get_status_copy(&status) == ESP_OK && status.mounted)
        {
            (void) device_sd_unmount();
        }
        return error;
    }

    s_running = true;
    ESP_LOGI(TAG, "SD 卡插拔监测 Service 已启动");
    return ESP_OK;
}

esp_err_t sd_card_service_stop(void)
{
    if (!s_running && !s_cleanup_failed)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = sd_card_service_task_stop();
    if (error != ESP_OK)
    {
        s_cleanup_failed = true;
        return error;
    }

    device_sd_status_t status;
    error = device_sd_get_status_copy(&status);
    if (error == ESP_OK && status.mounted)
    {
        error = device_sd_unmount();
        if (error == ESP_OK)
        {
            ESP_LOGI(TAG, "停机前 SD 卡文件系统已卸载");
        }
    }
    if (error != ESP_OK)
    {
        s_cleanup_failed = true;
        ESP_LOGE(TAG, "停止 SD 卡 Service 时文件系统未能收敛: %s", esp_err_to_name(error));
        return error;
    }

    s_running            = false;
    s_cleanup_failed     = false;
    s_last_present_valid = false;
    ESP_LOGI(TAG, "SD 卡插拔监测 Service 已停止");
    return ESP_OK;
}
