/*
 * 文件职责：把 Dashboard Store 邮件数据转换为邮箱页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化邮箱页 Presenter
 *
 * 建立初始空 View Model。
 *
 * @return ESP_OK 成功
 */
esp_err_t mail_presenter_init(void);

/**
 * @brief 从 Dashboard Store 同步刷新邮箱页 View Model
 *
 * @return ESP_OK 成功；其他值表示 Store 尚无有效邮件数据
 */
esp_err_t mail_presenter_refresh(void);

/**
 * @brief 复制邮箱页当前 View Model
 *
 * @param[out] out_view 接收邮箱页 View Model
 */
void mail_presenter_get_view_copy(mail_view_model_t *out_view);
