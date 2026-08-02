/*
 * 文件职责：把 Dashboard Store 限额数据转换为限额页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化限额页 Presenter
 *
 * 建立初始空 View Model。
 *
 * @return ESP_OK 成功
 */
esp_err_t quota_presenter_init(void);

/**
 * @brief 从 Dashboard Store 同步刷新限额页 View Model
 *
 * @return ESP_OK 成功；其他值表示 Store 尚无有效限额数据
 */
esp_err_t quota_presenter_refresh(void);

/**
 * @brief 将当前有效限额数据标记为过期
 *
 * 仅把 OK 状态改为 STALE；其他状态保持不变。
 */
void quota_presenter_set_stale(void);

/**
 * @brief 复制限额页当前 View Model
 *
 * @param[out] out_view 接收限额页 View Model
 */
void quota_presenter_get_view_copy(quota_view_model_t *out_view);
