/*
 * 文件职责：声明语音会话 Application 的产品意图接口。
 */
#pragma once

#include <stdbool.h>

#include "device_button.h"
#include "esp_err.h"

/**
 * @brief 初始化语音会话 Application
 *
 * 注册语音终态和可选唤醒词事件，初始化网络租约状态。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t app_voice_init(void);

/**
 * @brief 处理语音页按键事件
 *
 * 在语音页激活时，将按键事件转换为语音业务语义（如长按触发录音）。
 *
 * @param[in] key_event 按键事件
 * @return true 已处理；false 未处理（需上报给 App 输入入口）
 */
bool app_voice_consume_input(device_button_event_t key_event);
