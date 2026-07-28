/*
 * 文件职责：维护当前页面并编排页面导航产品用例。
 */
#pragma once

#include "device_button.h"
#include "esp_err.h"
#include "presentation_page.h"

/**
 * @brief 获取当前活跃页面 ID
 *
 * @return 当前页面 ID
 */
presentation_page_id_t app_page_get_current(void);

/**
 * @brief 查询顶层 Screen 是否仍在切换
 *
 * 超过 500 ms 未收到 UI 完成通知时自动解除门控并记录中文告警。
 *
 * @return true 切换尚未完成；false 当前可接收按键
 */
bool app_page_is_transitioning(void);

/**
 * @brief 接收 UI Screen 加载完成事实并解除匹配目标的按键门控
 *
 * @param[in] page 已完成加载的页面
 */
void app_page_reconcile_screen_loaded(presentation_page_id_t page);

/**
 * @brief 切换到下一页
 *
 * 环形导航，到达末尾后回到首页。
 *
 * @return ESP_OK 成功；其他值表示 UI 投递失败
 */
esp_err_t app_page_navigate_next(void);

/**
 * @brief 切换到上一页
 *
 * 环形导航，到达首页后跳到末尾。
 *
 * @return ESP_OK 成功；其他值表示 UI 投递失败
 */
esp_err_t app_page_navigate_previous(void);

/**
 * @brief 切换到指定页面
 *
 * 更新 App 状态、投递 UI 切换命令并上报 server。
 *
 * @param[in] page 目标页面 ID
 * @param[in] dir 导航方向，用于过渡动画
 * @return ESP_OK 成功；其他值表示 UI 投递失败
 */
esp_err_t app_page_navigate(presentation_page_id_t page, presentation_nav_dir_t dir);

/**
 * @brief 投递初始页面与状态栏呈现
 *
 * 系统启动时调用，投递首页切换和状态栏刷新命令。
 *
 * @return ESP_OK 成功
 */
esp_err_t app_page_dispatch_initial_presentation(void);

/**
 * @brief 认领并解释页面级按键输入
 *
 * 交互页优先解释输入；未被交互页认领的短按再转换为全局页面导航。
 *
 * @param[in] key_event 按键事件
 * @return true 已认领该输入事实；false 应继续由其他输入所有者解释
 */
bool app_page_consume_input(device_button_event_t key_event);
