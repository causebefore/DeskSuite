/*
 * 文件职责：定义限额页接口，展示 GLM 各项用量进度条阵列。
 * 主要依赖：LVGL、quota_presenter 提供的限额 view 数据。
 * 调用方：ui_router。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 初始化限额页 LVGL 控件（幂等，持锁调用）
 *
 * @return 始终为 ESP_OK
 */
esp_err_t ui_quota_page_init(void);

/**
 * @brief 显示限额页，展示 GLM 各项用量进度条阵列
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功展示；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_quota_page_show(lv_obj_t *parent);

/**
 * @brief 根据最新限额数据刷新限额页显示
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功刷新；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_quota_page_update(lv_obj_t *parent);
