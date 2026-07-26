/*
 * 文件职责：处理 App 输入业务命令。
 * 主要依赖：app_page、app_ota、button_service 事件。
 * 调用方：App 按键事件入口。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "device_button.h"

/**
 * @brief 初始化 App 输入处理模块
 *
 * 向 button_service 注册快速投递回调，并在默认事件循环中解释输入。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t app_key_init(void);

/**
 * @brief 处理 App 级按键事件
 *
 * App 输入入口，负责将按键事件分发给当前活跃页面的输入消费函数。
 * 处理全局逻辑（如页面切换）后，再交给页面处理。
 *
 * @param[in] key_event 按键事件
 * @return true 已处理；false 未处理
 */
bool app_key_dispatch_event(device_button_event_t key_event);
