/*
 * 文件职责：提供同步 UI 控件树初始化和消息渲染入口。
 * 主要依赖：ui_platform、ui_router、ui_status_bar。
 * 调用方：ui_task、状态栏模块。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "ui_types.h"

/**
 * @brief 在调用方持有 LVGL 锁时初始化 UI 层所有子模块
 *
 * 重复调用幂等：已初始化时直接返回 ESP_OK。任一子模块初始化失败时自动调用
 * ui_main_deinit 回滚已创建的控件，并转发子模块返回的错误码。
 *
 * @return ESP_OK 全部子模块初始化成功；其他值由失败的子模块返回
 */
esp_err_t ui_main_init(void);

/**
 * @brief 在调用方持有 LVGL 锁时销毁控件树并重置 UI 子模块
 *
 * @return ESP_OK 已完成
 */
esp_err_t ui_main_deinit(void);

/**
 * @brief 在调用方持有 LVGL 锁时从最新 View Model 重同步保留的控件树
 *
 * 本函数只更新控件状态，不直接唤醒 LVGL port 或提交显示刷新，适用于 Runtime
 * 从停止态恢复且 LVGL timer 尚未启动的阶段。
 *
 * @param[in] pending_page_switch 可选的最后一次页面切换消息；NULL 表示保留当前页面
 * @return ESP_OK 状态栏和当前页面已同步；ESP_ERR_INVALID_ARG 页面消息类型无效；
 *         ESP_ERR_INVALID_STATE 控件树尚未初始化；
 *         或 Presenter、页面刷新错误码
 */
esp_err_t ui_main_resync(const ui_msg_t *pending_page_switch);

/**
 * @brief 在调用方持有 LVGL 锁时同步处理一条 UI 消息
 *
 * @param[in] message 按值消息的只读地址
 * @return ESP_OK 已处理；ESP_ERR_NOT_SUPPORTED 表示当前消息没有渲染实现
 */
esp_err_t ui_main_handle_message(const ui_msg_t *message);

/**
 * @brief 获取状态栏容器对象
 *
 * @return lv_obj_t* 状态栏 LVGL 对象指针
 */
lv_obj_t *ui_main_get_status_bar(void);
