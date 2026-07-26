/*
 * 文件职责：把邮件领域快照转换为邮箱页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化邮箱页 Presenter
 *
 * 注册邮件事实事件并建立初始 View Model。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t mail_presenter_init(void);

/**
 * @brief 复制邮箱页当前 View Model
 *
 * @param[out] out_view 接收邮箱页 View Model
 */
void mail_presenter_get_view_copy(mail_view_model_t *out_view);
