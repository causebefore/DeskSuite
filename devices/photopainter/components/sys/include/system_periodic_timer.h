/**
 * @file system_periodic_timer.h
 * @brief 与具体 RTOS 无关的周期定时执行接口
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 周期定时器不透明句柄 */
    typedef struct system_periodic_timer *system_periodic_timer_handle_t;

    /**
     * @brief 周期定时回调
     *
     * 回调在 System 选择的定时执行上下文运行，必须快速返回，不得阻塞；具体 OS 或 RTOS
     * 任务类型不会通过本接口向 Service 泄漏。
     *
     * @param[in] elapsed_ms 距离上次回调实际经过的毫秒数，最小为 1
     * @param[in] context 创建定时器时借用的上下文
     */
    typedef void (*system_periodic_timer_callback_t)(uint32_t elapsed_ms, void *context);

    /** @brief 周期定时器创建配置 */
    typedef struct
    {
        const char                      *name;     /**< 诊断名称，借用至成功销毁 */
        system_periodic_timer_callback_t callback; /**< 回调，借用至成功销毁 */
        void                            *context;  /**< 回调上下文，借用至成功销毁 */
        bool skip_unhandled_events;                /**< true 表示调度延迟时合并积压的周期事件 */
    } system_periodic_timer_config_t;

    /**
     * @brief 创建一个尚未启动的周期定时器
     *
     * 成功后长期借用 config 中的 name、callback 和 context，直到
     * system_periodic_timer_destroy() 成功。
     *
     * @param[in] config 创建配置，仅结构本身在调用期间借用
     * @param[out] out_timer 新定时器句柄，仅在返回 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NO_MEM 内存不足；
     *         或底层执行环境错误码
     */
    esp_err_t system_periodic_timer_create_borrow(const system_periodic_timer_config_t *config,
                                                  system_periodic_timer_handle_t       *out_timer);

    /**
     * @brief 启动周期定时器
     *
     * @param[in] timer 已创建且尚未运行的定时器
     * @param[in] period_ms 周期毫秒数，必须大于 0
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 已运行；
     *         或底层执行环境错误码
     */
    esp_err_t system_periodic_timer_start(system_periodic_timer_handle_t timer, uint32_t period_ms);

    /**
     * @brief 停止正在运行的周期定时器
     *
     * @param[in] timer 正在运行的定时器
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 未运行；
     *         或底层执行环境错误码
     */
    esp_err_t system_periodic_timer_stop(system_periodic_timer_handle_t timer);

    /**
     * @brief 销毁已停止的周期定时器并结束配置借用
     *
     * @param[in] timer 已创建且未运行的定时器
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 仍在运行；
     *         或底层执行环境错误码
     */
    esp_err_t system_periodic_timer_destroy(system_periodic_timer_handle_t timer);

#ifdef __cplusplus
}
#endif
