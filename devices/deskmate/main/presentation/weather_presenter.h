/*
 * 文件职责：把 Dashboard Store 天气数据转换为天气页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化天气页 Presenter
 *
 * 建立初始空 View Model。
 *
 * @return ESP_OK 成功
 */
esp_err_t weather_presenter_init(void);

/**
 * @brief 从 Dashboard Store 同步刷新天气页 View Model
 *
 * @return ESP_OK 成功；其他值表示 Store 尚无有效天气数据
 */
esp_err_t weather_presenter_refresh(void);

/**
 * @brief 复制天气页当前 View Model
 *
 * @param[out] out_view 接收天气页 View Model
 */
void weather_presenter_get_view_copy(weather_view_model_t *out_view);
