/*
 * 文件职责：声明设置页产品意图接口。
 */
#pragma once

#include <stdbool.h>

#include "device_button.h"
#include "esp_err.h"

/**
 * @brief 请求进入配网 Portal
 *
 * 本函数只提交产品意图，最终切换结果由网络事实事件报告。
 *
 * @return ESP_OK 命令已提交；其他值表示当前状态拒绝或队列已满
 */
esp_err_t app_settings_request_portal(void);

/**
 * @brief 幂等清空设置菜单交互会话
 *
 * 顶层页面离开设置或 UI Runtime 重建时调用。函数先非阻塞提交网页控制台停止意图，并
 * 只在同步快照已经明确为 `APP_WEB_CONSOLE_STATE_STOPPED` 时继续丢弃尚未开始安装的 OTA
 * 目标和清除菜单门控；仍在启动、运行、停止或保留资源的错误态都会关闭失败，调用方必须
 * 保持当前页面并等待后续状态更新。配网 Portal 本身不受影响。
 *
 * @return ESP_OK 网页控制台已安全停止且会话、待安装目标已清理；
 *         ESP_ERR_INVALID_STATE 网页控制台尚未安全停止或停止序列不可用；
 *         其他值表示状态读取、停止意图提交或 OTA 目标清理失败
 */
esp_err_t app_settings_reset(void);

/**
 * @brief 解释设置页按键输入
 *
 * @param[in] key_event 按键事件
 * @return true 已消费；false 未消费
 */
bool app_settings_consume_input(device_button_event_t key_event);
