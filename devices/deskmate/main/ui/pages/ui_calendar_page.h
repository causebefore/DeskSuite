/*
 * 文件职责：定义日历页接口，显示 Presenter 提供的日程列表。
 * 主要依赖：LVGL、calendar_presenter 提供的日历 View Model。
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
 * @brief 创建日历页固定控件树并展示最新日程
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功展示；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_calendar_page_show(lv_obj_t *parent);

/**
 * @brief 仅更新文本与显隐以刷新日历页
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功刷新；ESP_ERR_INVALID_ARG parent 为空；ESP_ERR_INVALID_STATE 控件树不匹配
 */
esp_err_t ui_calendar_page_update(lv_obj_t *parent);
