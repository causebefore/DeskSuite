/*
 * 文件职责：定义 Toast 轻提示接口，供 App 通过 UI 命令触发。
 * 主要依赖：LVGL、UI 命令队列。
 * 调用方：ui_main 的 UI_MSG_SHOW_TOAST 处理流程。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 初始化 Toast 控件（幂等，持锁调用）
 *
 * @return ESP_OK 始终成功返回
 */
esp_err_t ui_toast_init(void);

/**
 * @brief 显示 Toast 轻提示，自动定时消失
 *
 * @param[in] text 提示文本内容
 * @return ESP_OK 始终成功返回
 */
esp_err_t ui_toast_show(const char *text);
