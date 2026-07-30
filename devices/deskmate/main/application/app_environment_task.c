/**
 * @file app_environment_task.c
 * @brief 显式拥有环境与电池产品采样周期
 */
#include "app_environment.h"

#include <stdbool.h>

#include "environment_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ENVIRONMENT_TASK_STACK               3072U
#define ENVIRONMENT_TASK_PRIORITY            2U
#define ENVIRONMENT_BATTERY_SAMPLE_PERIOD_MS 2000U
#define ENVIRONMENT_SENSOR_SAMPLE_PERIOD_MS  30000U

static const char *TAG = "app_environment_task";

static SemaphoreHandle_t s_stopped_sem;
static TaskHandle_t      s_task;
static bool              s_stopping;
static int64_t           s_next_battery_us;
static int64_t           s_next_environment_us;

static TickType_t ticks_until(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0)
    {
        return 0;
    }
    TickType_t ticks = pdMS_TO_TICKS((remaining_us + 999LL) / 1000LL);
    return ticks == 0 ? 1 : ticks;
}

static void reset_deadlines(void)
{
    const int64_t now_us  = esp_timer_get_time();
    s_next_battery_us     = now_us + (int64_t) ENVIRONMENT_BATTERY_SAMPLE_PERIOD_MS * 1000LL;
    s_next_environment_us = now_us + (int64_t) ENVIRONMENT_SENSOR_SAMPLE_PERIOD_MS * 1000LL;
}

static void sample_battery(void)
{
    const esp_err_t error = environment_service_sample_battery();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "提交电池采样尝试失败: %s", esp_err_to_name(error));
    }
    s_next_battery_us = esp_timer_get_time() + (int64_t) ENVIRONMENT_BATTERY_SAMPLE_PERIOD_MS * 1000LL;
}

static void sample_environment(void)
{
    const esp_err_t error = environment_service_sample_environment();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "提交温湿度采样尝试失败: %s", esp_err_to_name(error));
    }
    s_next_environment_us = esp_timer_get_time() + (int64_t) ENVIRONMENT_SENSOR_SAMPLE_PERIOD_MS * 1000LL;
}

static TickType_t wait_ticks_until_next_work(void)
{
    const int64_t next_us = s_next_battery_us < s_next_environment_us ? s_next_battery_us : s_next_environment_us;
    return ticks_until(next_us);
}

static void run_due_work(void)
{
    if (esp_timer_get_time() >= s_next_battery_us)
    {
        sample_battery();
    }
    if (esp_timer_get_time() >= s_next_environment_us)
    {
        sample_environment();
    }
}

static void app_environment_task(void *arg)
{
    (void) arg;
    reset_deadlines();

    for (;;)
    {
        if (ulTaskNotifyTake(pdTRUE, wait_ticks_until_next_work()) > 0U)
        {
            break;
        }
        run_due_work();
    }

    s_task = NULL;
    (void) xSemaphoreGive(s_stopped_sem);
    vTaskDelete(NULL);
}

esp_err_t app_environment_init(void)
{
    if (s_stopped_sem != NULL || s_task != NULL || s_stopping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_stopped_sem = xSemaphoreCreateBinary();
    if (s_stopped_sem == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreate(app_environment_task,
                                           "app_environment_task",
                                           ENVIRONMENT_TASK_STACK,
                                           NULL,
                                           ENVIRONMENT_TASK_PRIORITY,
                                           &s_task);
    if (created != pdPASS)
    {
        vSemaphoreDelete(s_stopped_sem);
        s_stopped_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "环境 Task 已启动，电池周期=%u ms，温湿度周期=%u ms",
             ENVIRONMENT_BATTERY_SAMPLE_PERIOD_MS,
             ENVIRONMENT_SENSOR_SAMPLE_PERIOD_MS);
    return ESP_OK;
}

esp_err_t app_environment_deinit(uint32_t timeout_ms)
{
    if (s_stopped_sem == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    const TickType_t started_ticks = xTaskGetTickCount();
    if (!s_stopping)
    {
        if (s_task == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }
        s_stopping = true;
        xTaskNotifyGive(s_task);
    }

    const TickType_t elapsed_ticks   = xTaskGetTickCount() - started_ticks;
    const TickType_t remaining_ticks = elapsed_ticks < timeout_ticks ? timeout_ticks - elapsed_ticks : 0;
    if (xSemaphoreTake(s_stopped_sem, remaining_ticks) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(s_stopped_sem);
    s_stopped_sem = NULL;
    s_stopping    = false;
    ESP_LOGI(TAG, "环境 Task 已协作停止");
    return ESP_OK;
}
