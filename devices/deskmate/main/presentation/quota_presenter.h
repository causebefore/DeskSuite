/*
 * 文件职责：把限额领域快照转换为限额页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化限额页 Presenter
 *
 * 注册限额事实事件并建立初始 View Model。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t quota_presenter_init(void);

/**
 * @brief 复制限额页当前 View Model
 *
 * @param[out] out_view 接收限额页 View Model
 */
void quota_presenter_get_view_copy(quota_view_model_t *out_view);
