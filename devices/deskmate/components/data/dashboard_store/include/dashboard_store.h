/*
 * 文件职责：缓存协议层已校验的 Dashboard 类型化结果，并提供线程安全切片读取。
 */
#pragma once

#include "deskmate_api.h"
#include "esp_err.h"

/**
 * @brief 初始化 Dashboard Store
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t dashboard_store_init(void);

/**
 * @brief 复制并提交协议层已校验的完整 Dashboard
 *
 * @param[in] dashboard 完整类型化 Dashboard，调用期间借用
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；
 *         ESP_ERR_INVALID_RESPONSE Dashboard 未通过协议校验；其他值表示初始化失败
 */
esp_err_t dashboard_store_update_copy(const deskmate_api_dashboard_result_t *dashboard);

/**
 * @brief 获取 Dashboard 天气数据副本
 *
 * @param[out] out 接收天气数据
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；
 *         ESP_ERR_INVALID_STATE Store 尚无有效天气数据
 */
esp_err_t dashboard_store_get_weather_copy(deskmate_api_dashboard_weather_t *out);

/**
 * @brief 获取 Dashboard 日历数据副本
 *
 * @param[out] out 接收日历数据
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；
 *         ESP_ERR_INVALID_STATE Store 尚无有效日历数据
 */
esp_err_t dashboard_store_get_calendar_copy(deskmate_api_dashboard_calendar_t *out);

/**
 * @brief 获取 Dashboard 邮件数据副本
 *
 * @param[out] out 接收邮件数据
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；
 *         ESP_ERR_INVALID_STATE Store 尚无有效邮件数据
 */
esp_err_t dashboard_store_get_mail_copy(deskmate_api_dashboard_mail_t *out);

/**
 * @brief 获取 Dashboard 限额数据副本
 *
 * @param[out] out 接收限额数据
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；
 *         ESP_ERR_INVALID_STATE Store 尚无有效限额数据
 */
esp_err_t dashboard_store_get_quota_copy(deskmate_api_dashboard_quota_t *out);
