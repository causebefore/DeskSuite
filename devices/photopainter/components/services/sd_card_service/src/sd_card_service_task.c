/**
 * @file sd_card_service_task.c
 * @brief 管理 SD 卡插拔中断延后处理 Task 的完整生命周期
 */
#include "sd_card_service_internal.h"

#include <stdint.h>

#include "device_sd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SD_CARD_SERVICE_TASK_STACK_SIZE 3072U
#define SD_CARD_SERVICE_TASK_PRIORITY   3U
#define SD_CARD_SERVICE_DEBOUNCE_MS     50U
#define SD_CARD_SERVICE_STOP_TIMEOUT_MS 500U
#define SD_CARD_SERVICE_NOTIFY_DETECT   (1UL << 0U)
#define SD_CARD_SERVICE_NOTIFY_STOP     (1UL << 1U)

/** @brief 日志标签 */
static const char *TAG = "sd_card_task";

/** @brief 插拔监测 Task 句柄，仅由本文件启停接口写入 */
static TaskHandle_t s_task;

/** @brief Task 退出握手信号量 */
static SemaphoreHandle_t s_task_stopped;

/** @brief 在 GPIO ISR 中快速通知 Service Task 处理卡状态变化 */
static void sd_card_service_on_detect_isr(void *context)
{
    (void) context;
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_task != NULL)
    {
        (void) xTaskNotifyFromISR(s_task,
                                  SD_CARD_SERVICE_NOTIFY_DETECT,
                                  eSetBits,
                                  &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE)
        {
            portYIELD_FROM_ISR();
        }
    }
}

/**
 * @brief 阻塞等待 SD 检测中断，去抖后串行执行挂载状态收敛
 *
 * Task 退出前先通知停止方，再挂起等待由停止方删除，避免句柄回收竞争。
 *
 * @param[in] context 未使用
 */
static void sd_card_service_task(void *context)
{
    (void) context;
    while (true)
    {
        uint32_t notification = 0U;
        (void) xTaskNotifyWait(0U, UINT32_MAX, &notification, portMAX_DELAY);
        if ((notification & SD_CARD_SERVICE_NOTIFY_STOP) != 0U)
        {
            break;
        }
        if ((notification & SD_CARD_SERVICE_NOTIFY_DETECT) != 0U)
        {
            vTaskDelay(pdMS_TO_TICKS(SD_CARD_SERVICE_DEBOUNCE_MS));
            const esp_err_t error = sd_card_service_reconcile_card();
            if (error != ESP_OK)
            {
                ESP_LOGW(TAG, "收敛 SD 卡插拔状态失败: %s", esp_err_to_name(error));
            }
        }
    }

    (void) xSemaphoreGive(s_task_stopped);
    vTaskSuspend(NULL);
}

esp_err_t sd_card_service_task_start(void)
{
    if (s_task_stopped != NULL || s_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_task_stopped = xSemaphoreCreateBinary();
    if (s_task_stopped == NULL)
    {
        ESP_LOGE(TAG, "创建 SD 卡 Task 停止信号量失败");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(sd_card_service_task,
                    "sd_card_service",
                    SD_CARD_SERVICE_TASK_STACK_SIZE,
                    NULL,
                    SD_CARD_SERVICE_TASK_PRIORITY,
                    &s_task)
        != pdPASS)
    {
        s_task = NULL;
        vSemaphoreDelete(s_task_stopped);
        s_task_stopped = NULL;
        ESP_LOGE(TAG, "创建 SD 卡插拔监测 Task 失败");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = device_sd_set_detect_isr_callback_borrow(sd_card_service_on_detect_isr, NULL);
    if (error != ESP_OK)
    {
        (void) xTaskNotify(s_task, SD_CARD_SERVICE_NOTIFY_STOP, eSetBits);
        if (xSemaphoreTake(s_task_stopped, pdMS_TO_TICKS(SD_CARD_SERVICE_STOP_TIMEOUT_MS))
            != pdTRUE)
        {
            ESP_LOGE(TAG, "回滚 SD 卡 Task 时等待停止超时");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelete(s_task);
        s_task = NULL;
        vSemaphoreDelete(s_task_stopped);
        s_task_stopped = NULL;
        ESP_LOGE(TAG, "注册 SD 卡检测 ISR 回调失败: %s", esp_err_to_name(error));
        return error;
    }

    (void) xTaskNotify(s_task, SD_CARD_SERVICE_NOTIFY_DETECT, eSetBits);
    return ESP_OK;
}

esp_err_t sd_card_service_task_stop(void)
{
    if (s_task_stopped == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task == NULL)
    {
        return ESP_OK;
    }

    esp_err_t error = device_sd_set_detect_isr_callback_borrow(NULL, NULL);
    if (error != ESP_OK)
    {
        return error;
    }

    (void) xTaskNotify(s_task, SD_CARD_SERVICE_NOTIFY_STOP, eSetBits);
    if (xSemaphoreTake(s_task_stopped, pdMS_TO_TICKS(SD_CARD_SERVICE_STOP_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "等待 SD 卡插拔监测 Task 停止超时");
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelete(s_task);
    s_task = NULL;
    vSemaphoreDelete(s_task_stopped);
    s_task_stopped = NULL;
    return ESP_OK;
}
