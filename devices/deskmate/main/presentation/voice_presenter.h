/*
 * 文件职责：把语音 Service 事实转换为语音页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/**
 * @brief 初始化语音页 Presenter
 *
 * 注册语音状态事件并建立初始 View Model。
 *
 * @return ESP_OK 成功；其他值表示事件注册失败
 */
esp_err_t voice_presenter_init(void);

/**
 * @brief 复制语音页当前 View Model
 *
 * @param[out] out_view 接收语音页 View Model
 */
void voice_presenter_get_view_copy(voice_view_model_t *out_view);
