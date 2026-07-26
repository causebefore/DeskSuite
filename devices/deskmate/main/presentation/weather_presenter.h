/*
 * 文件职责：把天气领域快照转换为天气页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化天气页 Presenter
 *
 * 注册天气事实事件并建立初始 View Model。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t weather_presenter_init(void);

/**
 * @brief 复制天气页当前 View Model
 *
 * @param[out] out_view 接收天气页 View Model
 */
void weather_presenter_get_view_copy(weather_view_model_t *out_view);
