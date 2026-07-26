/*
 * 文件职责：提供与业务无关的 LVGL/RLCD 运行时适配。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 初始化 LVGL port、显示对象和绘制缓冲
 *
 * @return ESP_OK 初始化成功；其他值表示平台资源创建或旧 Task 回收失败
 */
esp_err_t ui_platform_lvgl_init(void);

/**
 * @brief 同步暂停 LVGL 定时执行并等待显示传输静止
 *
 * 本函数保留 LVGL port Task、显示对象、绘制缓冲和底层显示资源。返回 ESP_OK 后，
 * 不会再提交新的显示刷新。
 *
 * @param[in] timeout_ms 获取 LVGL 锁和等待显示 DMA 的总超时，单位毫秒
 * @return ESP_OK 已停止或原本已停止；ESP_ERR_INVALID_ARG 超时无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化；或 LVGL/显示错误码
 */
esp_err_t ui_platform_lvgl_stop(uint32_t timeout_ms);

/**
 * @brief 恢复已停止的 LVGL 定时执行，并同步完成一次完整显示刷新
 *
 * 返回 ESP_OK 时，保留控件树的当前画面已经提交到显示设备并完成传输。
 *
 * @param[in] timeout_ms 获取 LVGL 锁、恢复显示和等待刷新完成的总超时，单位毫秒
 * @return ESP_OK 已恢复或原本正在运行；ESP_ERR_INVALID_ARG 超时无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化；或 LVGL/显示错误码
 */
esp_err_t ui_platform_lvgl_start(uint32_t timeout_ms);

/**
 * @brief 删除显示资源并等待 LVGL port Task 在期限内完全退出
 *
 * @param timeout_ms 获取 LVGL 锁和等待后台 Task 退出的总超时
 * @return ESP_OK 已完成；ESP_ERR_TIMEOUT 表示锁或 Task 退出超时
 */
esp_err_t ui_platform_lvgl_deinit(uint32_t timeout_ms);

/** @brief 有界获取唯一 LVGL 运行时锁 */
bool ui_platform_lvgl_lock(uint32_t timeout_ms);

/** @brief 释放唯一 LVGL 运行时锁 */
void ui_platform_lvgl_unlock(void);

/**
 * @brief 立即请求一帧；存在 LVGL 动画时自动保持连续刷新，否则提交后暂停
 *
 * 调用方必须持有 LVGL 锁，或本身运行在 taskLVGL 的 Timer/回调上下文。
 */
esp_err_t ui_platform_lvgl_request_refresh(void);

uint32_t ui_platform_lvgl_get_refresh_period(void);

uint32_t ui_platform_lvgl_get_flush_fps(void);
uint32_t ui_platform_lvgl_get_total_frames(void);
uint32_t ui_platform_lvgl_get_total_flush_count(void);
