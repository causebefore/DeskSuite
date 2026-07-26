/**
 * @file button_driver.h
 * @brief 与平台无关的按键消抖和事件识别状态机
 *
 * 本组件由外部 button-module 的状态机移植而来。它不访问 GPIO、不创建定时器，
 * 调用方负责读取电平并按实际调度间隔调用 button_driver_tick()。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 按键事件过滤掩码，默认启用全部事件 */
#define BUTTON_DRIVER_EVENT_MASK_ALL UINT32_MAX

    /** @brief 按键状态机产生的事件 */
    typedef enum
    {
        BUTTON_DRIVER_EVENT_PRESS = 0,
        BUTTON_DRIVER_EVENT_RELEASE,
        BUTTON_DRIVER_EVENT_CLICK,
        BUTTON_DRIVER_EVENT_DOUBLE_CLICK,
        BUTTON_DRIVER_EVENT_MULTI_CLICK,
        BUTTON_DRIVER_EVENT_LONG_PRESS_START,
        BUTTON_DRIVER_EVENT_LONG_PRESS_HOLD,
        BUTTON_DRIVER_EVENT_LONG_PRESS_END,
        BUTTON_DRIVER_EVENT_COUNT,
    } button_driver_event_t;

    /** @brief 按键状态机内部阶段 */
    typedef enum
    {
        BUTTON_DRIVER_STATE_IDLE = 0,
        BUTTON_DRIVER_STATE_DEBOUNCE_PRESS,
        BUTTON_DRIVER_STATE_DEBOUNCE_RELEASE,
        BUTTON_DRIVER_STATE_PRESSED,
        BUTTON_DRIVER_STATE_WAIT_NEXT_CLICK,
        BUTTON_DRIVER_STATE_LONG_PRESS,
    } button_driver_state_t;

    /**
 * @brief 读取按键原始电平的回调
 *
 * @param[in] context 调用方在配置中传入的上下文
 * @return 0 表示低电平，非 0 表示高电平
 */
    typedef int (*button_driver_read_level_cb_t)(void *context);

    struct button_driver;

    /**
 * @brief 按键事件回调
 *
 * 回调在 button_driver_tick() 的调用上下文执行，必须快速返回，不得重入同一实例。
 * button 指针只在回调期间借用，调用方不得保存。
 *
 * @param[in] button 产生事件的按键实例
 * @param[in] event 事件类型
 * @param[in] click_count 连击次数，仅 CLICK/DOUBLE_CLICK/MULTI_CLICK 事件有意义
 * @param[in] context 注册回调时传入的上下文
 */
    typedef void (*button_driver_event_cb_t)(const struct button_driver *button,
                                             button_driver_event_t event, uint8_t click_count,
                                             void *context);

    /** @brief 按键状态机配置 */
    typedef struct
    {
        uint32_t                      debounce_ms;
        uint32_t                      long_press_ms;
        uint32_t                      long_press_hold_ms;
        uint32_t                      multi_click_window_ms;
        uint32_t                      event_mask;
        uint8_t                       active_level;
        uint8_t                       max_click_count;
        button_driver_read_level_cb_t read_level;
        void                         *read_context;
    } button_driver_config_t;

    /**
 * @brief 可由调用方静态分配的按键状态机实例
 *
 * 字段属于驱动内部状态；调用方只负责实例存储期，不应直接修改字段。
 */
    typedef struct button_driver
    {
        button_driver_config_t   config;
        button_driver_event_cb_t event_callback;
        void                    *event_context;
        button_driver_state_t    state;
        uint32_t                 elapsed_ms;
        uint8_t                  click_count;
        uint8_t                  current_level;
        bool                     initialized;
        bool                     pressed;
        bool                     startup_skip;
        bool                     release_from_long_press;
    } button_driver_t;

    /**
 * @brief 填充推荐的默认时间配置
 *
 * 默认消抖 20 ms、长按 1000 ms、长按保持 100 ms、
 * 连击窗口 300 ms、最大连击 10 次；调用方仍需填写 read_level 和 read_context。
 *
 * @param[out] out_config 配置输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空
 */
    esp_err_t button_driver_get_default_config(button_driver_config_t *out_config);

    /**
 * @brief 初始化一个按键状态机实例
 *
 * 初始化时会读取一次实际电平；若按键正被按住，会等待释放后再开始识别，避免上电误触发。
 *
 * @param[out] out_button 调用方提供且已清零的实例存储
 * @param[in] config 配置，仅在调用期间借用
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 配置无效；ESP_ERR_INVALID_STATE 已初始化
 */
    esp_err_t button_driver_init(button_driver_t *out_button, const button_driver_config_t *config);

    /**
 * @brief 注册或清除事件回调
 *
 * 回调和 context 的借用持续到下一次设置或 button_driver_deinit()。
 *
 * @param[in,out] button 已初始化实例
 * @param[in] callback 回调；传入 NULL 表示清除
 * @param[in] context 回调上下文
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t button_driver_set_event_callback_borrow(button_driver_t         *button,
                                                      button_driver_event_cb_t callback,
                                                      void                    *context);

    /**
 * @brief 按实际经过时间执行一次同步扫描
 *
 * @param[in,out] button 已初始化实例
 * @param[in] elapsed_ms 距离上次扫描经过的毫秒数，必须大于 0
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空或时间无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t button_driver_tick(button_driver_t *button, uint32_t elapsed_ms);

    /**
 * @brief 查询当前已消抖的按下状态
 *
 * @param[in] button 已初始化实例
 * @param[out] out_pressed true 表示按键已确认按下
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t button_driver_is_pressed(const button_driver_t *button, bool *out_pressed);

    /**
 * @brief 释放按键状态机的回调借用并恢复未初始化状态
 *
 * @param[in,out] button 已初始化实例
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t button_driver_deinit(button_driver_t *button);

#ifdef __cplusplus
}
#endif
