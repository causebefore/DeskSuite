/*
 * 文件职责：定义主页接口，主页展示核心概览信息和入口卡片。
 * 主要依赖：LVGL、home_presenter、天气/日历等 Presentation View Model。
 * 调用方：ui_router。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 初始化主页 LVGL 控件（幂等，持锁调用）
 *
 * @return ESP_OK 主页初始化始终成功
 */
esp_err_t ui_home_page_init(void);

/**
 * @brief 在主页控件删除后清空增量刷新句柄
 */
void ui_home_page_deinit(void);

/**
 * @brief 显示主页，展示核心概览信息和入口卡片
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 主页布局创建并完成首次填充；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_home_page_show(lv_obj_t *parent);

/**
 * @brief 根据最新数据刷新主页显示
 *
 * 仅更新动态文本和图标，不创建或销毁控件。
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 主页已按最新 view 完成增量刷新
 * @return ESP_ERR_INVALID_STATE 尚未调用 _show 创建布局（s_created 为 false）
 */
esp_err_t ui_home_page_update(lv_obj_t *parent);
