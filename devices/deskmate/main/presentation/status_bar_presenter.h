/*
 * 文件职责：聚合状态栏所需事实并生成只读 View Model。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "presentation_page.h"

/** @brief 状态栏标题最大长度（含 '\0'） */
#define STATUS_BAR_VIEW_TITLE_MAX 16

/** @brief 状态栏 View Model */
typedef struct
{
    char    page_title[STATUS_BAR_VIEW_TITLE_MAX]; /*!< 当前页面标题 */
    bool    wifi_connected;                        /*!< Wi-Fi 是否已连接 */
    bool    wifi_connecting;                       /*!< Wi-Fi 正在连接（状态栏图标闪烁提示） */
    bool    time_valid;                            /*!< 时间是否有效 */
    uint8_t hour;                                  /*!< 当前小时 (0-23) */
    uint8_t minute;                                /*!< 当前分钟 (0-59) */
    bool    battery_valid;                         /*!< 电池信息是否有效 */
    uint8_t battery_percent;                       /*!< 电池电量 (%) */
    bool    server_online;                         /*!< 服务器连接是否正常 */
} status_bar_view_model_t;

/**
 * @brief 初始化状态栏 Presenter
 *
 * 订阅电池和系统时钟事件；网络事实在读取 View Model 时从 Communication 快照刷新。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t status_bar_presenter_init(void);

/**
 * @brief 复制状态栏当前 View Model
 *
 * 聚合电池、网络、时间等服务状态到 out_view，供 UI 层读取渲染。
 *
 * @param[out] out_view 接收状态栏 View Model
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出指针为空
 */
esp_err_t status_bar_presenter_get_view_copy(status_bar_view_model_t *out_view);

/**
 * @brief 根据当前页面更新状态栏标题
 *
 * @param[in] page 当前页面 ID
 * @return true 标题发生变化；false 标题保持不变
 */
bool status_bar_presenter_set_page(presentation_page_id_t page);

/**
 * @brief 更新 DeskMate 服务端可达状态
 *
 * 该事实由网络 Application 在 Dashboard 事务完成后提供。
 *
 * @param[in] online true 表示最近一次 Dashboard 同步成功
 */
void status_bar_presenter_set_server_online(bool online);
