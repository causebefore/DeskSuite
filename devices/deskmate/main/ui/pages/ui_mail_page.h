/*
 * 文件职责：定义邮箱页接口，显示邮件列表和未读状态。
 * 主要依赖：LVGL、mail_presenter 提供的邮箱状态数据。
 * 调用方：ui_router。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 初始化邮箱页 LVGL 控件（幂等，持锁调用）
 *
 * @return ESP_OK 邮箱页初始化始终成功
 */
esp_err_t ui_mail_page_init(void);

/**
 * @brief 创建邮箱页固定控件树并展示最新邮件摘要
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 邮箱页已创建并完成首次绘制；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_mail_page_show(lv_obj_t *parent);

/**
 * @brief 仅更新文本与显隐以刷新邮箱页
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功刷新；ESP_ERR_INVALID_ARG parent 为空；ESP_ERR_INVALID_STATE 控件树不匹配
 */
esp_err_t ui_mail_page_update(lv_obj_t *parent);
