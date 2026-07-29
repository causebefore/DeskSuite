/*
 * 文件职责：提供 Composition Root 初始化和启动入口，让 main.c 保持精简。
 * 主要依赖：Application、Presentation、UI 和运行时能力。
 * 调用方：main.c 的 app_main。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 初始化产品各层及其运行时依赖
 *
 * 该入口由顶层 main.c 调用，按依赖顺序初始化本地数据、输入、Application、Presentation
 * 和 UI Runtime 固定资源。为缩短首屏等待，音频 Device、AFE 和语音 Service 延后到
 * app_main_start() 的首屏派发之后初始化。
 *
 * @return ESP_OK 初始化成功；其他错误来自业务服务或 ui_main。
 */
esp_err_t app_main_init(void);

/**
 * @brief 启动产品运行期组件
 *
 * 该入口先启动 UI 并派发首屏，再完成 RTC、番茄钟、音频/语音运行时、轻睡眠 Application
 * 和按键扫描。按键只在全部页面依赖就绪后开放，避免输入进入未启动的产品能力。
 *
 * @return ESP_OK 启动成功；其他错误来自 ui_main 或首屏派发。
 */
esp_err_t app_main_start(void);
