/**
 * @file app_environment.h
 * @brief 声明环境与电池产品周期 Task 及按需采样命令
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 创建环境产品采样调度 Task
 *
 * @return ESP_OK 已创建；ESP_ERR_INVALID_STATE 已存在实例；ESP_ERR_NO_MEM 资源不足
 */
esp_err_t app_environment_init(void);

/**
 * @brief 协作停止 Task 并释放自身队列与停止回执
 *
 * 超时时保留现有资源和 STOPPING 状态，调用方可再次调用继续等待。
 *
 * @param[in] timeout_ms 最长等待时间，单位毫秒
 * @return ESP_OK 已停止并释放；ESP_ERR_INVALID_STATE 尚未初始化；ESP_ERR_TIMEOUT 未及时退出
 */
esp_err_t app_environment_deinit(uint32_t timeout_ms);

/**
 * @brief 请求尽快执行一次电池采样
 *
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 当前不可接收；ESP_ERR_TIMEOUT 队列已满
 */
esp_err_t app_environment_request_battery_sample(void);

/**
 * @brief 请求尽快执行一次温湿度采样
 *
 * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 当前不可接收；ESP_ERR_TIMEOUT 队列已满
 */
esp_err_t app_environment_request_environment_sample(void);
