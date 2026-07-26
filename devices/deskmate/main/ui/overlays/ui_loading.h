/*
 * 文件职责：定义 Loading 状态接口，供网络拉取、OTA 等流程显示忙碌状态。
 * 主要依赖：LVGL、UI 命令队列。
 * 调用方：ui_main 的 UI_MSG_SHOW_LOADING / UI_MSG_HIDE_LOADING 处理流程。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 初始化 Loading 控件（幂等，持锁调用）
 *
 * @return ESP_OK 始终成功返回
 */
esp_err_t ui_loading_init(void);

/**
 * @brief 显示 Loading 状态，用于网络拉取、OTA 等流程
 *
 * @return ESP_OK 始终成功返回
 */
esp_err_t ui_loading_show(void);

/**
 * @brief 隐藏 Loading 状态
 *
 * @return ESP_OK 始终成功返回
 */
esp_err_t ui_loading_hide(void);
