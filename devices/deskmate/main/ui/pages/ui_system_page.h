/*
 * 文件职责：声明设置菜单内的系统信息子页接口。
 * 主要依赖：system_presenter、ui_common。
 * 调用方：ui_settings_page。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 初始化系统页 LVGL 控件（幂等，持锁调用）
 *
 * @return 始终为 ESP_OK
 */
esp_err_t ui_system_page_init(void);

/**
 * @brief 显示版本、构建、运行时长、内存和 CPU 信息
 *
 * @param[in] parent 页面内容容器
 * @return ESP_OK 成功展示；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_system_page_show(lv_obj_t *parent);

/**
 * @brief 根据最新系统状态刷新系统页显示
 *
 * @param[in] parent 页面内容容器
 * @return ESP_OK 成功刷新；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_system_page_update(lv_obj_t *parent);
