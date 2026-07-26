/**
 * @file environment_service.h
 * @brief 按需采集温湿度与电池状态，并向上层提供一致快照
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 最近一次成功的温湿度数据及最近一次尝试结果 */
    typedef struct
    {
        bool      valid;            /**< 是否至少成功采集过一次 */
        float     temperature_c;    /**< 最近一次成功采集的温度，单位摄氏度 */
        float     humidity_percent; /**< 最近一次成功采集的相对湿度百分比 */
        uint64_t  updated_at_ms;    /**< 最近一次成功采集时的单调时间，单位 ms */
        esp_err_t last_error;       /**< 最近一次温湿度采集结果 */
    } environment_service_environment_status_t;

    /** @brief 最近一次成功的电池数据及最近一次尝试结果 */
    typedef struct
    {
        bool      valid;         /**< 是否至少成功采集过一次 */
        uint32_t  voltage_mv;    /**< 最近一次成功采集的电池电压，单位 mV */
        float     percent;       /**< 最近一次成功换算的电量百分比 */
        uint64_t  updated_at_ms; /**< 最近一次成功采集时的单调时间，单位 ms */
        esp_err_t last_error;    /**< 最近一次电池采集结果 */
    } environment_service_battery_status_t;

    /** @brief 温湿度与电池联合状态快照 */
    typedef struct
    {
        environment_service_environment_status_t environment; /**< 温湿度状态 */
        environment_service_battery_status_t     battery;     /**< 电池状态 */
        uint64_t last_attempt_at_ms; /**< 最近一次联合采样开始时的单调时间，单位 ms */
        uint64_t sample_count;       /**< 已完成的联合采样次数 */
    } environment_service_status_t;

    /**
     * @brief 初始化环境 Service 的联合快照资源
     *
     * 调用前必须已初始化 `device_environment` 与 `device_battery`。本函数不采样硬件。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；ESP_ERR_NO_MEM 无法创建内部资源
     */
    esp_err_t environment_service_init(void);

    /**
     * @brief 同步执行一次按需温湿度与电池联合采样
     *
     * 本函数会阻塞约 210 ms，并串行化并发采样请求。单项硬件采样失败仍会发布本次尝试，
     * 具体结果通过 `environment_service_get_status_copy()` 中两项的 `valid` 和
     * `last_error` 判断；此时函数仍返回 ESP_OK。
     *
     * @return ESP_OK 本次尝试已发布；ESP_ERR_INVALID_STATE 尚未初始化；ESP_FAIL 内部锁失败
     */
    esp_err_t environment_service_sample(void);

    /**
     * @brief 复制最近一次温湿度与电池联合状态
     *
     * 本函数只短暂等待内部状态锁，不触发硬件 I/O。调用方应分别检查两项 `valid` 和
     * `last_error`；采样失败时，有效数据字段保留最近一次成功值。
     *
     * @param[out] out_status 状态快照，仅 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t environment_service_get_status_copy(environment_service_status_t *out_status);

    /**
     * @brief 释放环境 Service 内部资源
     *
     * 本函数不释放 `device_environment` 与 `device_battery`，其生命周期仍由装配方负责。
     * 生命周期控制 API 必须由装配方串行调用，且释放期间不得并发读取快照。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t environment_service_deinit(void);

#ifdef __cplusplus
}
#endif
