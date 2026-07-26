/*
 * 文件职责：定义天气页接口，显示当前天气和未来天气数据。
 * 主要依赖：LVGL、weather 经 App 转换后的状态数据。
 * 调用方：ui_router。
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief 初始化天气页 LVGL 控件（幂等，持锁调用）
 *
 * @return ESP_OK 天气页初始化始终成功
 */
esp_err_t ui_weather_page_init(void);

/**
 * @brief 显示天气页，创建页面控件并绑定数据
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 天气页布局创建并完成首次填充；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_weather_page_show(lv_obj_t *parent);

/**
 * @brief 在天气页 Screen 删除后清空动态控件句柄
 */
void ui_weather_page_deinit(void);

/**
 * @brief 根据最新天气数据刷新天气页显示
 *
 * 只改文本和可见性，不重建控件，跑马灯动画不被打断。
 *
 * @param[in] parent 当前 Screen 的页面内容容器
 * @return ESP_OK 天气页已按最新 view 完成增量刷新；ESP_ERR_INVALID_ARG parent 为空
 */
esp_err_t ui_weather_page_update(lv_obj_t *parent);
