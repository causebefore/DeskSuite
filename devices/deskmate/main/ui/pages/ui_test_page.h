/*
 * 文件职责：定义开发阶段测试页接口，用于展示传感器、事件和调试状态。
 * 主要依赖：LVGL、App 调试状态、各 Service 的只读诊断数据。
 * 调用方：ui_router。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 初始化测试页 LVGL 控件（幂等，持锁调用）
 *
 * @return 始终为 ESP_OK
 */
esp_err_t ui_test_page_init(void);

/**
 * @brief 显示测试页，展示传感器、事件和调试状态
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功展示；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_test_page_show(lv_obj_t *parent);

/**
 * @brief 根据最新调试数据刷新测试页显示
 *
 * 测试页为静态字体标本，无需按数据刷新。
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return 始终为 ESP_OK
 */
esp_err_t ui_test_page_update(lv_obj_t *parent);
