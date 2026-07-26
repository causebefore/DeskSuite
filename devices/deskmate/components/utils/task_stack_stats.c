/**
 * @file task_stack_stats.c
 * @brief 实现 FreeRTOS 任务栈高水位的低频串口统计
 */
#include "task_stack_stats.h"

#include "esp_log.h"

/** @brief 常驻任务栈统计输出周期，避免高频执行任务刷屏 */
#define TASK_STACK_STATS_LOG_INTERVAL_MS 60000U

void task_stack_stats_log_now(const char *task_name)
{
    if (task_name == NULL)
    {
        return;
    }
    ESP_LOGI(TASK_STACK_STATS_LOG_TAG,
             "任务栈统计: task=%s, 历史最小剩余=%lu 字节",
             task_name,
             (unsigned long) uxTaskGetStackHighWaterMark(NULL));
}

void task_stack_stats_log_if_due(task_stack_stats_t *stats, const char *task_name)
{
    if (stats == NULL)
    {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (stats->logged_once && (now - stats->last_log_tick) < pdMS_TO_TICKS(TASK_STACK_STATS_LOG_INTERVAL_MS))
    {
        return;
    }

    task_stack_stats_log_now(task_name);
    stats->last_log_tick = now;
    stats->logged_once   = true;
}
