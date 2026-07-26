/**
 * @file sd_card_service.h
 * @brief SD 卡插拔监测与自动挂载 Service
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 启动 SD 卡插拔监测 Task 并收敛当前卡状态
     *
     * 调用前必须已成功初始化 device_sd。
     *
     * Service 通过 GPIO ISR 通知唤醒 Task，去抖后自动执行挂载或卸载。当前已插卡但挂载失败
     * 不阻止监测启动，失败事实通过日志和 device_sd 状态报告。
     *
     * @return ESP_OK 已启动；ESP_ERR_INVALID_STATE 已运行或清理失败；
     *         或 Task、GPIO 回调注册错误码
     */
    esp_err_t sd_card_service_start(void);

    /**
     * @brief 同步停止监测 Task 并卸载已挂载文件系统
     *
     * 最长等待 500 ms。若 Task 或文件系统未达到终态，保留清理失败状态并拒绝重新启动，
     * 调用方可以再次调用本函数继续收敛。
     *
     * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE 未运行；
     *         ESP_ERR_TIMEOUT Task 未按时退出；或卸载错误码
     */
    esp_err_t sd_card_service_stop(void);

#ifdef __cplusplus
}
#endif
