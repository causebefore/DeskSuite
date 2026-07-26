/*
 * 文件职责：定义设置概览与两层设置菜单的 UI 接口。
 * 主要依赖：LVGL、Presentation 设置动作。
 * 调用方：ui_router、ui_main。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "presentation_dispatch.h"

/**
 * @brief 初始化设置页 LVGL 控件（幂等，持锁调用）
 *
 * @return 始终为 ESP_OK
 */
esp_err_t ui_settings_page_init(void);

/**
 * @brief 显示顶层设置概览
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功展示；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_settings_page_show(lv_obj_t *parent);

/**
 * @brief 在设置页 Screen 删除后清空动态控件和交互资源
 */
void ui_settings_page_deinit(void);

/**
 * @brief 根据最新 Presenter 快照刷新当前设置视图
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 已刷新；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_settings_page_update(lv_obj_t *parent);

/**
 * @brief 将物理按键映射出的设置动作交给 LVGL 菜单处理
 *
 * @param[in] action 设置菜单动作
 * @return ESP_OK 动作已处理或按规则忽略；其他值表示业务意图被拒绝
 */
esp_err_t ui_settings_page_handle_action(presentation_settings_action_t action);
