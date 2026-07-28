/*
 * 文件职责：声明 Presentation 事件及其不可变载荷。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "presentation_page.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Presentation 事件 ID */
typedef enum
{
    PRESENTATION_EVENT_PAGE_SWITCH = 0,   /*!< 页面切换 */
    PRESENTATION_EVENT_STATUS_BAR_UPDATE, /*!< 仅状态栏 View Model 变化 */
    PRESENTATION_EVENT_STATUS_UPDATE,     /*!< 当前页面或状态栏 View Model 变化 */
    PRESENTATION_EVENT_POMODORO_UPDATE,   /*!< 番茄钟页或状态栏 View Model 变化 */
    PRESENTATION_EVENT_OTA_UPDATE,        /*!< OTA View Model 变化 */
    PRESENTATION_EVENT_SETTINGS_ACTION,   /*!< 设置菜单物理按键动作 */
} presentation_event_id_t;

/** @brief 设置菜单可解释的物理按键动作 */
typedef enum
{
    PRESENTATION_SETTINGS_ACTION_OPEN = 0, /*!< 从设置概览打开菜单 */
    PRESENTATION_SETTINGS_ACTION_PREV,     /*!< 聚焦上一项 */
    PRESENTATION_SETTINGS_ACTION_NEXT,     /*!< 聚焦下一项 */
    PRESENTATION_SETTINGS_ACTION_ACTIVATE, /*!< 激活当前项或当前子页动作 */
    PRESENTATION_SETTINGS_ACTION_BACK,     /*!< 返回上一层或关闭菜单 */
} presentation_settings_action_t;

/** @brief 页面切换事件载荷 */
typedef struct
{
    presentation_page_id_t page; /*!< 目标页面 */
    presentation_nav_dir_t dir;  /*!< 页面过渡方向 */
} presentation_page_switch_event_t;

/** @brief 设置菜单动作事件载荷 */
typedef struct
{
    presentation_settings_action_t action; /*!< 按值复制的设置动作 */
} presentation_settings_action_event_t;

/** @brief Presentation 事件基类 */
ESP_EVENT_DECLARE_BASE(PRESENTATION_EVENT);

/**
 * @brief 发布页面切换事件
 *
 * @param[in] page 目标页面
 * @param[in] dir 页面过渡方向
 * @return ESP_OK 已发布；ESP_ERR_INVALID_ARG 参数无效；其他值表示事件队列错误
 */
esp_err_t presentation_dispatch_page_switch(presentation_page_id_t page, presentation_nav_dir_t dir);

/**
 * @brief 发布状态栏 View Model 变化事件
 *
 * @return ESP_OK 已发布；其他值表示事件队列错误
 */
esp_err_t presentation_dispatch_status_bar_update(void);

/**
 * @brief 发布当前页面或状态栏 View Model 变化事件
 *
 * @return ESP_OK 已发布；其他值表示事件队列错误
 */
esp_err_t presentation_dispatch_status_update(void);

/**
 * @brief 发布番茄钟页面或状态栏 View Model 变化事件
 *
 * @return ESP_OK 已发布；其他值表示事件队列错误
 */
esp_err_t presentation_dispatch_pomodoro_update(void);

/**
 * @brief 发布 OTA View Model 变化事件
 *
 * @return ESP_OK 已发布；其他值表示事件队列错误
 */
esp_err_t presentation_dispatch_ota_update(void);

/**
 * @brief 发布一条设置菜单动作
 *
 * @param[in] action OPEN/PREV/NEXT/ACTIVATE/BACK 之一
 * @return ESP_OK 已发布；ESP_ERR_INVALID_ARG action 无效；其他值表示事件队列错误
 */
esp_err_t presentation_dispatch_settings_action(presentation_settings_action_t action);

#ifdef __cplusplus
}
#endif
