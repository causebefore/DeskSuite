/*
 * 文件职责：管理 UI 页面注册、切换和页面刷新入口。
 * 主要依赖：LVGL、ui_types、各 ui_xxx_page 模块。
 * 调用方：ui_main、Presentation 发布的 UI_MSG_SWITCH_PAGE 处理流程。
 */
#pragma once

#include "esp_err.h"
#include "ui_types.h"

/**
 * @brief 初始化页面路由器，注册所有页面模块（幂等，持锁调用）
 *
 * @return ESP_OK 全部页面初始化成功；其他值由失败的页面 _init 返回
 */
esp_err_t ui_router_init(void);

/**
 * @brief 取消 Screen 动画并清空页面模块对 LVGL 控件的借用句柄
 */
void ui_router_deinit(void);

/**
 * @brief 切换到指定页面，按导航方向播放过渡动画
 *
 * 为目标页面按需创建独立 Screen；前进/后退使用 LVGL 原生 MOVE 动画，
 * 导航方向为 NONE 时立即加载。旧 Screen 在动画完成后由 LVGL 自动删除。
 *
 * @param[in] page 目标页面 ID
 * @param[in] dir 导航方向（FORWARD/BACKWARD/NONE）
 * @return ESP_OK 切换成功；ESP_ERR_INVALID_ARG 未知页面 ID；
 *         ESP_ERR_NO_MEM 创建 Screen 失败；其他值由目标页面 _show 返回
 */
esp_err_t ui_router_switch_to(ui_page_id_t page, ui_nav_dir_t dir);

/**
 * @brief 刷新当前页面数据
 *
 * @return ESP_OK 刷新成功；ESP_ERR_INVALID_ARG 当前页面 ID 未知；其他值由当前页面 _update 返回
 */
esp_err_t ui_router_refresh_current(void);
