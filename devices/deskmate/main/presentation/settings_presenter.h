/*
 * 文件职责：把网络连接事实转换为设置页 View Model。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "presentation_view_model.h"

/** @brief 设置页展示的 Wi-Fi 连接阶段 */
typedef enum
{
    SETTINGS_NETWORK_VIEW_IDLE = 0,   /*!< 尚未开始连接或配网 */
    SETTINGS_NETWORK_VIEW_CONNECTING, /*!< 正在连接已保存的 Wi-Fi */
    SETTINGS_NETWORK_VIEW_CONNECTED,  /*!< Wi-Fi 已连接 */
    SETTINGS_NETWORK_VIEW_FAILED,     /*!< 最近一次 Wi-Fi 连接失败 */
    SETTINGS_NETWORK_VIEW_PORTAL,     /*!< 配网 Portal 已启动 */
} settings_network_view_state_t;

/** @brief 设置页 View Model */
typedef struct
{
    settings_network_view_state_t network_state;                              /*!< 当前网络阶段 */
    bool                          wifi_connected;                             /*!< Wi-Fi 是否已连接 */
    char                          station_ssid[PRESENTATION_PORTAL_SSID_MAX]; /*!< 当前连接或最近尝试的 Wi-Fi */
    char                          station_ip[16];                             /*!< 当前 IPv4 地址 */
    int8_t                        rssi_dbm;                                   /*!< 当前信号强度 (dBm) */
    char                          current_version[24];                        /*!< 当前固件版本 */
    settings_portal_view_model_t  portal;                                     /*!< 配网门户信息 */
} settings_view_model_t;

/**
 * @brief 初始化设置页 Presenter
 *
 * 建立初始 View Model；后续读取时查询 Network Manager 与 Connect 快照。
 *
 * @return ESP_OK 成功
 */
esp_err_t settings_presenter_init(void);

/**
 * @brief 复制设置页当前 View Model
 *
 * @param[out] out_view 接收设置页 View Model
 */
void settings_presenter_get_view_copy(settings_view_model_t *out_view);
