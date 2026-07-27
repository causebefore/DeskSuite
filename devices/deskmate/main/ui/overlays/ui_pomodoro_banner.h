/**
 * @file ui_pomodoro_banner.h
 * @brief 声明跨页面番茄钟完成提示的持久快照同步接口
 */
#pragma once

#include "esp_err.h"
#include "presentation_page.h"

/** @brief 创建并隐藏全局完成提示；@return ESP_OK 已创建，或资源错误 */
esp_err_t ui_pomodoro_banner_init(void);
/** @brief 删除提示 Timer 并清空控件句柄 */
void ui_pomodoro_banner_deinit(void);

/**
 * @brief 从 Presenter latch 同步全局完成提示
 *
 * 同一 completion_generation 最多显示一次；番茄钟页内完成会直接标记为已展示。
 *
 * @param[in] current_page 当前页面
 * @return ESP_OK 已同步；其他值由 Presenter 返回
 */
esp_err_t ui_pomodoro_banner_sync(presentation_page_id_t current_page);
