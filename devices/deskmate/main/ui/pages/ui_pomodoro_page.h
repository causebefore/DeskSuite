/**
 * @file ui_pomodoro_page.h
 * @brief 声明番茄钟页面的创建、增量刷新与句柄清理接口
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/** @brief 初始化番茄钟页面；@return ESP_OK 初始化完成 */
esp_err_t ui_pomodoro_page_init(void);
/** @brief 清空随番茄钟 Screen 删除而失效的控件句柄 */
void ui_pomodoro_page_deinit(void);

/**
 * @brief 在当前内容容器创建番茄钟页面
 *
 * @param[in] parent 当前 Screen 的 400×268 内容容器
 * @return ESP_OK 已创建；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_pomodoro_page_show(lv_obj_t *parent);

/**
 * @brief 按最新 Presenter 快照增量刷新番茄钟页面
 *
 * @param[in] parent 当前 Screen 的内容容器
 * @return ESP_OK 已刷新；ESP_ERR_INVALID_STATE 尚未创建；ESP_ERR_INVALID_ARG容器不匹配
 */
esp_err_t ui_pomodoro_page_update(lv_obj_t *parent);
