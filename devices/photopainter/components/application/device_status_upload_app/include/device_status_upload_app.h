/**
 * @file device_status_upload_app.h
 * @brief 设备温湿度与电池状态上传用例
 */
#pragma once

#include "esp_err.h"
#include "protocol_backend_context.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 单次设备状态上传配置 */
    typedef struct
    {
        const protocol_backend_context_t *backend;    /**< 完整后端上下文，调用期间借用 */
        int                               timeout_ms; /**< HTTP 请求超时，单位 ms */
    } device_status_upload_app_config_t;

    /**
     * @brief 读取最新环境联合快照并同步上传一次
     *
     * 电池最近一次采样必须成功；温湿度最近一次采样失败时省略 environment，仍上传电池。
     * 本函数不触发硬件采样，不保存配置指针，也不负责网络重试或周期调度。
     *
     * @param[in] config 单次上传配置
     * @return ESP_OK 服务端已接受；ESP_ERR_INVALID_ARG 配置无效；ESP_ERR_INVALID_STATE
     *         环境 Service 或电池状态不可用；或最近电池采样、网络和 HTTP 错误码
     */
    esp_err_t device_status_upload_app_upload(const device_status_upload_app_config_t *config);

#ifdef __cplusplus
}
#endif
