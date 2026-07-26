/**
 * @file environment_service.h
 * @brief 统一环境与电池按需采样、快照和更新通知
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(ENVIRONMENT_SERVICE_EVENT);

    /** @brief Service 只发布轻量通知，消费者再通过 getter 拉取快照 */
    typedef enum
    {
        ENVIRONMENT_SERVICE_EVENT_ENVIRONMENT_UPDATED = 0,
        ENVIRONMENT_SERVICE_EVENT_BATTERY_UPDATED,
    } environment_service_event_t;

    /** @brief 环境采样状态 */
    typedef struct
    {
        int16_t   temperature_centi; /*!< 温度，单位 0.01°C */
        uint16_t  humidity_centi;    /*!< 湿度，单位 0.01% */
        bool      valid;             /*!< 是否存在最近成功值 */
        esp_err_t last_error;        /*!< 最近一次环境采样结果 */
        uint64_t  updated_at_ms;     /*!< 最近成功更新时间 */
    } environment_service_environment_status_t;

    /** @brief 电池采样状态 */
    typedef struct
    {
        uint16_t  voltage_mv;    /*!< 电池电压，单位毫伏 */
        uint8_t   percent;       /*!< 电量百分比 */
        bool      low;           /*!< 是否低电量 */
        bool      valid;         /*!< 是否存在最近成功值 */
        esp_err_t last_error;    /*!< 最近一次电池采样结果 */
        uint64_t  updated_at_ms; /*!< 最近成功更新时间 */
    } environment_service_battery_status_t;

    /** @brief 最近一次完整 Service 快照 */
    typedef struct
    {
        environment_service_environment_status_t environment;
        environment_service_battery_status_t     battery;
        uint64_t                                 last_attempt_at_ms;
        uint64_t                                 sample_count;
    } environment_service_status_t;

    /**
     * @brief 创建快照锁与采样事务锁
     *
     * 本函数不初始化 Device，也不创建 Task 或 Timer。
     */
    esp_err_t environment_service_init(void);

    /**
     * @brief 在调用者上下文同步执行一次环境与电池联合采样
     *
     * 返回 ESP_OK 表示尝试结果已经提交；每个硬件项目是否成功由快照中的
     * `valid` 与 `last_error` 独立表示。
     */
    esp_err_t environment_service_sample(void);

    /** @brief 在调用者上下文同步执行一次温湿度采样 */
    esp_err_t environment_service_sample_environment(void);

    /** @brief 在调用者上下文同步执行一次电池采样 */
    esp_err_t environment_service_sample_battery(void);

    /** @brief 复制最近快照，不触发硬件 I/O */
    esp_err_t environment_service_get_status_copy(environment_service_status_t *out_status);

    /**
     * @brief 释放 Service 自身锁与快照
     *
     * 本函数不释放 `device_environment` 或 `device_battery`。
     */
    esp_err_t environment_service_deinit(void);

#ifdef __cplusplus
}
#endif
