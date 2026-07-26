/**
 * @file device_button.h
 * @brief 声明不暴露 GPIO 的双按键消抖和长短按能力
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DEVICE_BUTTON_MAX_EVENTS 2U

    /** @brief 产品按键事件类型 */
    typedef enum
    {
        DEVICE_BUTTON_EVENT_NONE = 0,    /*!< 无事件 */
        DEVICE_BUTTON_EVENT_LEFT_SHORT,  /*!< 左键短按 */
        DEVICE_BUTTON_EVENT_LEFT_LONG,   /*!< 左键长按 */
        DEVICE_BUTTON_EVENT_RIGHT_SHORT, /*!< 右键短按 */
        DEVICE_BUTTON_EVENT_RIGHT_LONG,  /*!< 右键长按 */
    } device_button_event_t;

    /** @brief GPIO ISR 上下文中的按键活动回调 */
    typedef void (*device_button_activity_callback_t)(void *context);

    /** @brief 一轮双按键扫描的有界结果 */
    typedef struct
    {
        device_button_event_t events[DEVICE_BUTTON_MAX_EVENTS]; /**< 本轮稳定事件 */
        uint8_t               event_count;                      /**< 有效事件数量 */
        bool                  follow_up_required;               /**< 是否仍需定时推进 */
    } device_button_scan_result_t;

    /** @brief 双按键的当前物理按下状态快照 */
    typedef struct
    {
        bool left_pressed;  /**< 左键当前是否为低电平按下 */
        bool right_pressed; /**< 右键当前是否为低电平按下 */
    } device_button_pressed_state_t;

    /**
 * @brief 初始化双按键和内部状态机
 * @param debounce_ms 消抖时间，单位毫秒
 * @param long_press_ms 长按阈值，单位毫秒
 * @return ESP_OK 成功；其他值表示参数或 BSP 初始化失败
 */
    esp_err_t device_button_init(uint32_t debounce_ms, uint32_t long_press_ms);

    /**
     * @brief 推进左右键状态机并复制本轮完整结果
     * @param[in] now_ms 当前单调时间，单位毫秒
     * @param[out] out_result 本轮最多两个事件和继续推进事实
     * @return ESP_OK 已完成扫描；其他值表示尚未初始化、参数错误或 BSP 读取失败
     */
    esp_err_t device_button_scan(uint32_t now_ms, device_button_scan_result_t *out_result);

    /**
     * @brief 注册或清除长期借用的按键活动回调
     *
     * 非 NULL 回调运行在 GPIO ISR 上下文，只允许执行 ISR-safe 的有界操作。
     *
     * @param[in] callback GPIO ISR 上下文的活动回调；NULL 表示清除
     * @param[in] context 长期借用的上下文；清除时忽略
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
     */
    esp_err_t device_button_set_activity_callback_borrow(device_button_activity_callback_t callback, void *context);

    /**
     * @brief 复制双按键当前物理按下状态
     *
     * 本函数只读取 GPIO 电平，不推进消抖或长短按状态机。用于轻睡眠返回后判定已释放的唤醒按键
     * 是否需要由 Application 重放为短按事实。
     *
     * @param[out] out_state 按下状态快照，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或 BSP 读取错误
     */
    esp_err_t device_button_get_pressed_state_copy(device_button_pressed_state_t *out_state);

    /**
 * @brief 释放按键 BSP 资源并重置状态
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
 */
    esp_err_t device_button_deinit(void);

#ifdef __cplusplus
}
#endif
