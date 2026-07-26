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
 * 该入口由顶层 main.c 调用，按依赖顺序初始化下层能力、Application、Presentation 和 UI
 * Runtime。函数只完成初始化阶段，显式启动阶段由 app_main_start() 完成。
 *
 * @return ESP_OK 初始化成功；其他错误来自业务服务或 ui_main。
 */
esp_err_t app_main_init(void);

/**
 * @brief 启动产品运行期组件
 *
 * 该入口会启动 UI、轻睡眠 Application、按键扫描和网络周期策略。
 *
 * @return ESP_OK 启动成功；其他错误来自 ui_main 或首屏派发。
 */
esp_err_t app_main_start(void);
