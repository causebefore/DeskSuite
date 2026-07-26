/**
 * @file device_button.h
 * @brief 与板型和 GPIO 无关的设备按键能力
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 设备对外暴露的三个按键 */
    typedef enum
    {
        DEVICE_BUTTON_LEFT = 0,
        DEVICE_BUTTON_RIGHT,
        DEVICE_BUTTON_CONFIRM,
        DEVICE_BUTTON_COUNT,
    } device_button_id_t;

    /** @brief 设备按键事件 */
    typedef enum
    {
        DEVICE_BUTTON_EVENT_PRESS = 0,
        DEVICE_BUTTON_EVENT_RELEASE,
        DEVICE_BUTTON_EVENT_CLICK,
        DEVICE_BUTTON_EVENT_DOUBLE_CLICK,
        DEVICE_BUTTON_EVENT_MULTI_CLICK,
        DEVICE_BUTTON_EVENT_LONG_PRESS_START,
        DEVICE_BUTTON_EVENT_LONG_PRESS_HOLD,
        DEVICE_BUTTON_EVENT_LONG_PRESS_END,
    } device_button_event_t;

    /**
 * @brief 设备按键事件回调
 *
 * 回调在 device_button_scan() 的调用上下文同步执行，必须快速返回；耗时业务应复制参数后
 * 投递到其所有者队列。回调不得重入 device_button_* 控制 API。
 *
 * @param[in] button 按键标识
 * @param[in] event 事件类型
 * @param[in] click_count 连击次数，仅点击类事件有意义
 * @param[in] context 注册时传入的上下文
 */
    typedef void (*device_button_event_cb_t)(device_button_id_t button, device_button_event_t event,
                                             uint8_t click_count, void *context);

    /**
 * @brief 初始化设备按键能力
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或 BSP 错误码
 */
    esp_err_t device_button_init(void);

    /**
 * @brief 注册或清除设备按键事件回调
 *
 * 不得与 device_button_scan() 并发调用。回调与 context 的借用持续到下一次设置或
 * device_button_deinit()，以先发生者为准。
 *
 * @param[in] callback 回调；传入 NULL 表示清除
 * @param[in] context 回调上下文
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t device_button_set_event_callback_borrow(device_button_event_cb_t callback,
                                                      void                    *context);

    /**
 * @brief 同步采样按键并推进一次设备事件状态机
 *
 * 本函数不等待、不创建 Task、Queue 或 Timer；持续调度由 Service 负责。事件回调在本函数
 * 调用上下文同步执行。
 *
 * @param[in] elapsed_ms 距离上次采样经过的毫秒数，必须大于 0
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 时间无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
 */
    esp_err_t device_button_scan(uint32_t elapsed_ms);

    /**
 * @brief 查询指定按键当前已消抖的按下状态
 *
 * @param[in] button 按键标识
 * @param[out] out_pressed true 表示按下
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t device_button_is_pressed(device_button_id_t button, bool *out_pressed);

    /**
 * @brief 释放设备按键资源
 *
 * 不得与 device_button_scan() 并发调用。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 BSP 错误码
 */
    esp_err_t device_button_deinit(void);

#ifdef __cplusplus
}
#endif
