/*
 * 文件职责：定义顶部常驻状态栏接口，显示页面名、时间、Wi-Fi 和电池信息。
 * 主要依赖：LVGL、status_bar_presenter 视图数据。
 * 调用方：ui_main、ui_router、UI_MSG_STATUS_UPDATE 处理流程。
 */
#pragma once

#include "status_bar_presenter.h"
#include "esp_err.h"

/**
 * @brief 初始化状态栏控件
 *
 * 必须在 ui_main create_root_layout() 之后调用。
 *
 * @return ESP_OK 状态栏控件创建成功
 * @return ESP_ERR_INVALID_STATE 状态栏父容器尚未就绪
 */
esp_err_t ui_status_bar_init(void);

/**
 * @brief 在状态栏控件删除后清空句柄和数据缓存
 */
void ui_status_bar_deinit(void);

/**
 * @brief 根据最新状态快照刷新状态栏
 *
 * 刷新 WiFi 图标、电池电量、时间等状态栏信息。
 *
 * @param[in] status 状态栏视图数据快照
 * @return ESP_OK 刷新成功
 * @return ESP_ERR_INVALID_STATE 状态栏控件尚未创建
 * @return ESP_ERR_INVALID_ARG status 为 NULL
 */
esp_err_t ui_status_bar_update(const status_bar_view_model_t *status);
