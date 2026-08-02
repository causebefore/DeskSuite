/*
 * 文件职责：定义语音交互页接口，显示语音状态和操作提示。
 * 主要依赖：LVGL、voice_presenter 提供的语音状态数据。
 * 调用方：ui_router。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 初始化语音交互页 LVGL 控件（幂等，持锁调用）
 *
 * @return ESP_OK 语音交互页初始化始终成功
 */
esp_err_t ui_voice_page_init(void);

/**
 * @brief 显示语音交互页，展示语音状态和操作提示
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 语音交互页已创建并完成首次绘制；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_voice_page_show(lv_obj_t *parent);

/**
 * @brief 仅更新文本与显隐以刷新语音交互页
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 成功刷新；ESP_ERR_INVALID_ARG parent 为空；ESP_ERR_INVALID_STATE 控件树不匹配
 */
esp_err_t ui_voice_page_update(lv_obj_t *parent);
