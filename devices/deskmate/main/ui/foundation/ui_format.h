/*
 * 文件职责：纯格式化函数（时间/日期/星期/运行时长/内存/RSSI），无 LVGL 依赖，可 host 测试。
 * 主要依赖：Presentation View Model 类型。
 * 调用方：主页、系统页。
 *
 * 贴合规范「逻辑与平台分离」：本文件只做 snprintf，不碰 LVGL/ESP-IDF 运行时。
 */
#pragma once

#include "presentation_view_model.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 格式化时间为 HH:MM 字符串
 *
 * @param[in] time 时间视图数据
 * @param[out] out 输出缓冲区
 * @param[in] out_len 缓冲区长度
 */
void ui_format_time(const home_time_view_model_t *time, char *out, size_t out_len);

/**
 * @brief 格式化主页日期字符串（如 "6月29日 周一"）
 *
 * @param[in] time 时间视图数据
 * @param[out] out 输出缓冲区
 * @param[in] out_len 缓冲区长度
 */
void ui_format_home_date(const home_time_view_model_t *time, char *out, size_t out_len);

/**
 * @brief 根据年月日获取星期名称
 *
 * @param[in] year 年份
 * @param[in] month 月份（1-12）
 * @param[in] day 日期（1-31）
 * @return const char* 星期名称字符串（如 "周一"）
 */
const char *ui_format_weekday(uint16_t year, uint8_t month, uint8_t day);

/**
 * @brief 格式化运行时长为 "Xd Xh Xm" 字符串
 *
 * @param[in] sec 秒数
 * @param[out] out 输出缓冲区
 * @param[in] out_len 缓冲区长度
 */
void ui_format_uptime(uint32_t sec, char *out, size_t out_len);

/**
 * @brief 格式化内存信息为 "used/total (xx%)" 字符串
 *
 * @param[in] free_kb 空闲内存（KB）
 * @param[in] total_kb 总内存（KB）
 * @param[in] used_percent 使用百分比
 * @param[out] out 输出缓冲区
 * @param[in] out_len 缓冲区长度
 */
void ui_format_memory(uint32_t free_kb, uint32_t total_kb, uint8_t used_percent, char *out, size_t out_len);

/**
 * @brief 格式化 RSSI 信号强度为文字描述
 *
 * @param[in] dbm 信号强度（dBm）
 * @param[out] out 输出缓冲区
 * @param[in] out_len 缓冲区长度
 */
void ui_format_rssi(int8_t dbm, char *out, size_t out_len);
