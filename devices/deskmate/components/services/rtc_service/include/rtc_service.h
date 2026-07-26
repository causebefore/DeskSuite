/**
 * @file rtc_service.h
 * @brief 持续消费 RTC INT，并在普通 Task 上下文发布告警事件
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief RTC Service 生命周期状态 */
    typedef enum
    {
        RTC_SERVICE_STATE_UNINITIALIZED = 0,
        RTC_SERVICE_STATE_INITIALIZED,
        RTC_SERVICE_STATE_STARTING,
        RTC_SERVICE_STATE_RUNNING,
        RTC_SERVICE_STATE_STOPPING,
    } rtc_service_state_t;

    /** @brief RTC Service 对上报告的事件类型 */
    typedef enum
    {
        RTC_SERVICE_EVENT_ALARM_TRIGGERED = 1,
        RTC_SERVICE_EVENT_PROCESSING_FAILED,
    } rtc_service_event_id_t;

    /** @brief RTC Service 不可变事件 */
    typedef struct
    {
        rtc_service_event_id_t id;          /**< 事件类型 */
        esp_err_t              result;      /**< 本次 AF 读取/清除结果 */
        uint32_t               alarm_count; /**< 本轮运行累计成功消费的告警数 */
    } rtc_service_event_t;

    /** @brief RTC Service 状态快照 */
    typedef struct
    {
        rtc_service_state_t state;
        uint32_t            alarm_count;
        esp_err_t           last_error;
    } rtc_service_status_t;

    /**
     * @brief RTC 告警事件回调
     *
     * 回调在 RTC Service Task 上下文同步执行，event 只在回调期间有效。回调必须快速返回，
     * 不得保存 event 地址或执行无界阻塞。
     */
    typedef void (*rtc_service_event_callback_t)(const rtc_service_event_t *event, void *context);

    /**
     * @brief 初始化尚未运行的 RTC 中断消费 Service
     *
     * 本函数不初始化 `device_rtc`，其生命周期由 Composition Root 管理。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；ESP_ERR_NO_MEM 资源不足
     */
    esp_err_t rtc_service_init(void);

    /**
     * @brief 在停止状态设置长期借用的事件回调
     *
     * 传入 NULL 清除回调。借用期持续到再次设置、`rtc_service_deinit()` 或调用方主动清除。
     *
     * @param[in] callback 事件回调，可为 NULL
     * @param[in] context 原样传给回调的上下文
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 生命周期不允许
     */
    esp_err_t rtc_service_set_event_callback_borrow(rtc_service_event_callback_t callback, void *context);

    /**
     * @brief 启动 RTC INT 消费 Task
     *
     * 返回前完成 ISR 回调注册，并主动检查启动前已经置位的 AF。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 生命周期不允许；或资源错误
     */
    esp_err_t rtc_service_start(void);

    /**
     * @brief 请求 Service Task 检查当前告警标志
     *
     * 用于调用方需要主动核对 AF、而没有 GPIO 下降沿通知的场景。返回值只表示通知是否提交，
     * 实际 I2C 结果通过事件和状态快照报告。
     *
     * @return ESP_OK 通知已提交；ESP_ERR_INVALID_STATE Service 未运行
     */
    esp_err_t rtc_service_request_check(void);

    /**
     * @brief 注销 ISR 回调并同步停止 RTC Service Task
     *
     * @param[in] timeout_ms 等待 Task 终止的上限，必须大于 0
     * @return ESP_OK 已停止；ESP_ERR_TIMEOUT 未在期限内停止，此时状态保留为 STOPPING；
     *         ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 生命周期不允许
     */
    esp_err_t rtc_service_stop(uint32_t timeout_ms);

    /**
     * @brief 复制 RTC Service 状态快照
     * @param[out] out_status 快照，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空
     */
    esp_err_t rtc_service_get_status_copy(rtc_service_status_t *out_status);

    /**
     * @brief 释放已停止的 RTC Service
     *
     * 本函数不释放 `device_rtc`。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE Service 仍在运行或未初始化
     */
    esp_err_t rtc_service_deinit(void);

#ifdef __cplusplus
}
#endif
