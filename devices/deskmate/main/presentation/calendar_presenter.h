/*
 * 文件职责：把 Dashboard Store 日历数据转换为日历页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化日历页 Presenter
 *
 * 建立初始空 View Model。
 *
 * @return ESP_OK 成功
 */
esp_err_t calendar_presenter_init(void);

/**
 * @brief 从 Dashboard Store 同步刷新日历页 View Model
 *
 * @return ESP_OK 成功；其他值表示 Store 尚无有效日历数据
 */
esp_err_t calendar_presenter_refresh(void);

/**
 * @brief 将当前有效日历数据标记为过期
 *
 * 仅把 OK 状态改为 STALE；其他状态保持不变。
 */
void calendar_presenter_set_stale(void);

/**
 * @brief 复制日历页当前 View Model
 *
 * @param[out] out_view 接收日历页 View Model
 */
void calendar_presenter_get_view_copy(calendar_view_model_t *out_view);
