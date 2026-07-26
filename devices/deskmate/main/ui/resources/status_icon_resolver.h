/*
 * 文件职责：把状态栏业务状态映射到通用静态图片资源。
 * 主要依赖：LVGL、ui_platform 图片目录。
 * 调用方：ui_status_bar。
 */
#pragma once

#include "lvgl.h"

/**
 * @brief 状态栏图标资源 ID
 *
 * 与 status_icon_resolver_get() 配合，按业务状态取对应静态图片：
 * Wi-Fi 在线/离线、服务端在线/离线、五档电量图标。
 */
typedef enum
{
    STATUS_ICON_WIFI_ONLINE = 0, /*!< Wi-Fi 已连接 */
    STATUS_ICON_WIFI_OFFLINE,    /*!< Wi-Fi 未连接 */
    STATUS_ICON_SERVER_ONLINE,   /*!< 服务端可达 */
    STATUS_ICON_SERVER_OFFLINE,  /*!< 服务端不可达 */
    STATUS_ICON_BATTERY_0,       /*!< 电量 0~5% */
    STATUS_ICON_BATTERY_25,      /*!< 电量 6~25% */
    STATUS_ICON_BATTERY_50,      /*!< 电量 26~50% */
    STATUS_ICON_BATTERY_75,      /*!< 电量 51~75% */
    STATUS_ICON_BATTERY_100,     /*!< 电量 76~100% */
} status_icon_id_t;

/**
 * @brief 按图标 ID 查询状态栏静态图片资源
 *
 * @param icon 图标 ID
 * @return const lv_image_dsc_t* 对应图片描述符；ID 越界时返回 NULL
 */
const lv_image_dsc_t *status_icon_resolver_get(status_icon_id_t icon);
