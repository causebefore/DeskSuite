/*
 * 文件职责：定义日历页接口，显示日期、日程和提醒信息。
 * 主要依赖：LVGL、calendar 经 App 转换后的状态数据。
 * 调用方：ui_router。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 初始化日历页 LVGL 控件（幂等，持锁调用）
 *
 * @return 始终为 ESP_OK
 */
esp_err_t ui_calendar_page_init(void);

/**
 * @brief 显示日历页，展示日期、日程和提醒信息
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功展示；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_calendar_page_show(lv_obj_t *parent);

/**
 * @brief 根据最新日历数据刷新日历页显示
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功刷新；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_calendar_page_update(lv_obj_t *parent);
