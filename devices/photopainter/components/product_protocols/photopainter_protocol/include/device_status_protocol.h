/**
 * @file device_status_protocol.h
 * @brief 设备温湿度与电池状态上传协议
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 设备状态上传数据 */
    typedef struct
    {
        bool     has_environment;    /**< true 表示本轮温湿度测量有效 */
        float    temperature_c;      /**< 温度，单位 °C，仅 has_environment 时有效 */
        float    humidity_percent;   /**< 相对湿度，单位 %RH，仅 has_environment 时有效 */
        float    battery_percent;    /**< 电池电量百分比 */
        uint32_t battery_voltage_mv; /**< 电池电压，单位 mV */
    } device_status_protocol_upload_t;

    /**
     * @brief 同步上传设备最近一次有效的温湿度与电池状态
     *
     * device_id 为空时不发送 `X-Device-Id`，由服务端使用默认设备 ID。所有输入指针只在
     * 调用期间借用，函数返回后不会保存。
     *
     * @param[in] in_base_url 服务端基础地址
     * @param[in] in_token 可为空的设备共享令牌
     * @param[in] in_device_id 可为空的设备 ID
     * @param[in] in_status 待上传状态
     * @param[in] timeout_ms HTTP 请求超时，单位 ms
     * @return ESP_OK 上传成功；ESP_ERR_INVALID_ARG 参数或数值无效；或网络、HTTP、内存错误码
     */
    esp_err_t device_status_protocol_upload_borrow(const char *in_base_url, const char *in_token,
                                                   const char *in_device_id,
                                                   const device_status_protocol_upload_t *in_status,
                                                   int timeout_ms);

#ifdef __cplusplus
}
#endif
