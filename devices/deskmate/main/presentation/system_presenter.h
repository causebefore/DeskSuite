/*
 * 文件职责：把系统诊断事实转换为系统信息页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/** @brief 系统信息页 View Model */
typedef struct
{
    system_info_view_model_t system; /*!< 系统诊断信息 */
} system_page_view_model_t;

/**
 * @brief 初始化系统页 Presenter
 *
 * 建立初始 View Model；后续读取时查询系统信息快照。
 *
 * @return ESP_OK 成功
 */
esp_err_t system_presenter_init(void);

/**
 * @brief 复制系统页当前 View Model
 *
 * @param[out] out_view 接收系统页 View Model
 */
void system_presenter_get_view_copy(system_page_view_model_t *out_view);
