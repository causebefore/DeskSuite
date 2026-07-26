/**
 * @file display_present_service.h
 * @brief 单张 PPF2 页面读取、校验与墨水屏呈现 Service
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 单个 ASCII 状态页允许的最大文本行数 */
#define DISPLAY_PRESENT_SERVICE_ASCII_LINE_MAX 4U

/** @brief ASCII 状态页中的一行坐标式文本 */
typedef struct
{
    uint16_t x_pixels; /**< 文本左上角横坐标 */
    uint16_t y_pixels; /**< 文本左上角纵坐标 */
    const char *text;  /**< 同步呈现期间借用的 ASCII 文本 */
    uint8_t scale;     /**< 5x7 字模整数缩放倍数 */
} display_present_service_ascii_line_t;

/** @brief 呈现 Service 状态快照 */
typedef struct
{
    bool      busy; /**< 正在执行同步呈现 */
    char      last_page_id[DISPLAY_PROTOCOL_PAGE_ID_MAX]; /**< 最近成功呈现页面 */
    esp_err_t last_error; /**< 最近一次呈现错误 */
} display_present_service_status_t;

/**
 * @brief 初始化呈现 Service 的互斥资源和单页 PSRAM 缓冲区
 *
 * 调用前必须已以四灰阶模式初始化 device_display。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化或显示模式不匹配；
 *         ESP_ERR_NO_MEM 资源不足；或显示状态查询错误码
 */
esp_err_t display_present_service_init(void);

/**
 * @brief 同步读取、校验并呈现一个本地 PPF2 页面
 *
 * file_path 和 expected_page 只在调用期间借用。函数完成时墨水屏全局刷新已经结束。
 *
 * @param[in] file_path SD 卡相对页面路径
 * @param[in] expected_page Manifest 中的预期页面元数据
 * @return ESP_OK 呈现成功；ESP_ERR_INVALID_STATE 尚未初始化或已有呈现事务；
 *         ESP_ERR_INVALID_ARG 参数无效；或 SD、协议、显示错误码
 */
esp_err_t display_present_service_present_borrow(const char *file_path,
                                                 const display_protocol_page_t *expected_page);

/**
 * @brief 串行等待现有页面呈现结束，再居中显示 ASCII 状态文本
 *
 * 本函数与页面呈现共用同一互斥事务。获得所有权后清白内部帧、合成文本并完成一次物理全刷；
 * 不修改最近成功页面 ID。调用最长等待 20 秒取得呈现所有权，随后物理刷新最长约 15 秒。
 *
 * @param[in] text 非空 ASCII 状态文本，仅在调用期间借用
 * @param[in] scale 5x7 字模整数缩放倍数，必须大于 0
 * @return ESP_OK 呈现成功；ESP_ERR_TIMEOUT 等待现有呈现结束超时；
 *         ESP_ERR_INVALID_STATE 尚未初始化；或文本、显示错误码
 */
esp_err_t display_present_service_present_ascii_centered_borrow(const char *text, uint8_t scale);

/**
 * @brief 串行等待现有页面呈现结束，再一次性呈现多行坐标式 ASCII 状态页
 *
 * 获得呈现所有权后清白内部帧，按数组顺序绘制全部文本行并只执行一次物理全刷。
 * lines、每个 text 与其上下文只在同步调用期间借用，不修改最近成功页面 ID。
 *
 * @param[in] lines 一至 DISPLAY_PRESENT_SERVICE_ASCII_LINE_MAX 行文本
 * @param[in] line_count 文本行数
 * @return ESP_OK 呈现成功；ESP_ERR_TIMEOUT 等待现有呈现结束超时；
 *         ESP_ERR_INVALID_ARG 行数组、数量或文本无效；或显示错误码
 */
esp_err_t display_present_service_present_ascii_layout_borrow(
    const display_present_service_ascii_line_t *lines, size_t line_count);

/**
 * @brief 复制当前呈现状态
 *
 * @param[out] out_status 状态输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t display_present_service_get_status_copy(display_present_service_status_t *out_status);

/**
 * @brief 释放呈现 Service 的 PSRAM 缓冲区与互斥资源
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或仍在呈现
 */
esp_err_t display_present_service_deinit(void);

#ifdef __cplusplus
}
#endif
