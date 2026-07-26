/**
 * @file device_battery.h
 * @brief 与板型和 ADC 细节无关的设备电池监测能力
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 一次电池状态采样结果 */
    typedef struct
    {
        uint32_t voltage_mv; /**< 经 ADC 校准和分压补偿后的电池电压，单位 mV */
        float    percent;    /**< 按官方放电曲线换算并限制在 0～100 的电量百分比 */
    } device_battery_status_t;

    /**
     * @brief 初始化设备电池监测能力
     *
     * 初始化不持续开启电池监测电路，也不会创建 Task。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或 BSP 初始化错误码
     */
    esp_err_t device_battery_init(void);

    /**
     * @brief 同步采集电池电压并换算电量百分比
     *
     * 调用会临时开启电池监测电路并阻塞约 200 ms，只能在 Task 上下文串行调用。
     *
     * @param[out] out_status 电池状态快照，仅 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或 BSP 采样错误码
     */
    esp_err_t device_battery_get_status_copy(device_battery_status_t *out_status);

    /**
     * @brief 释放设备电池监测资源
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 清理错误码
     */
    esp_err_t device_battery_deinit(void);

#ifdef __cplusplus
}
#endif
