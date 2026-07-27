/*
 * 文件职责：定义 UI 页面别名和 Presentation 到 UI 的命令消息类型。
 */
#pragma once

#include <stdint.h>

#include "presentation_page.h"

/** @brief UI 页面 ID 类型，复用 Presentation 页面契约 */
typedef presentation_page_id_t ui_page_id_t;

/** @brief UI 页面导航方向类型，复用 Presentation 页面契约 */
typedef presentation_nav_dir_t ui_nav_dir_t;

/**
 * @brief Presentation 到 UI 的命令消息类型枚举
 */
typedef enum
{
    UI_MSG_NONE = 0,          /*!< 无操作 */
    UI_MSG_SWITCH_PAGE,       /*!< 切换页面 */
    UI_MSG_STATUS_BAR_UPDATE, /*!< 更新状态栏布局 */
    UI_MSG_STATUS_UPDATE,     /*!< 刷新状态栏数据（WiFi、电池、时间等） */
    UI_MSG_POMODORO_UPDATE,   /*!< 刷新番茄钟状态栏角标和当前番茄钟页 */
    UI_MSG_SHOW_TOAST,        /*!< 显示 Toast 轻提示 */
    UI_MSG_SHOW_LOADING,      /*!< 显示 Loading 状态 */
    UI_MSG_HIDE_LOADING,      /*!< 隐藏 Loading 状态 */
    UI_MSG_OTA_UPDATE,        /*!< OTA View Model 更新 */
    UI_MSG_SETTINGS_ACTION,   /*!< 设置菜单按键动作 */
} ui_msg_type_t;

/**
 * @brief UI 命令消息结构体
 */
typedef struct
{
    ui_msg_type_t type;  /*!< 消息类型 */
    ui_page_id_t  page;  /*!< 目标页面（切换页面时使用） */
    uint32_t      param; /*!< 固定宽度附加参数，如导航方向或设置动作 */
} ui_msg_t;
