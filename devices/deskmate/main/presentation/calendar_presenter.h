/*
 * 文件职责：把日历领域快照转换为日历页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化日历页 Presenter
 *
 * 注册日历事实事件并建立初始 View Model。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t calendar_presenter_init(void);

/**
 * @brief 复制日历页当前 View Model
 *
 * @param[out] out_view 接收日历页 View Model
 */
void calendar_presenter_get_view_copy(calendar_view_model_t *out_view);
