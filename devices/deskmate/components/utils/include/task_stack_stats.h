/**
 * @file task_stack_stats.h
 * @brief 提供 FreeRTOS 任务栈高水位的低频串口统计
 */
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 单个任务持有的栈统计节流状态
     */
    typedef struct
    {
        TickType_t last_log_tick; /*!< 上次输出日志的系统 Tick */
        bool       logged_once;   /*!< 是否已经输出过首次统计 */
    } task_stack_stats_t;

    /** @brief 任务栈统计状态初始化值 */
#define TASK_STACK_STATS_INITIALIZER { 0U, false }

    /** @brief 所有任务栈统计统一使用的 ESP-IDF 日志标签 */
#define TASK_STACK_STATS_LOG_TAG     "task_stack"

    /**
     * @brief 首次立即输出，之后按固定周期输出当前任务的栈高水位
     *
     * `uxTaskGetStackHighWaterMark(NULL)` 在 ESP-IDF 中返回任务启动以来历史最小剩余栈，
     * 单位为字节。调用方应在自身 Task 上下文中调用本函数。
     *
     * @param[in,out] stats 任务私有的统计节流状态
     * @param[in] task_name 串口日志中展示的稳定任务名
     */
    void task_stack_stats_log_if_due(task_stack_stats_t *stats, const char *task_name);

    /**
     * @brief 立即输出当前任务的栈高水位，不受周期限制
     *
     * @param[in] task_name 串口日志中展示的稳定任务名
     */
    void task_stack_stats_log_now(const char *task_name);

#ifdef __cplusplus
}
#endif
