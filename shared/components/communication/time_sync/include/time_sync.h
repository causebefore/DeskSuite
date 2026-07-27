/**
 * @file time_sync.h
 * @brief 提供与产品校时策略无关的 SNTP 网络取样
 */
#pragma once

#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief 一次 SNTP 网络样本
 *
 * 该结构只描述网络取样事实，不表示时间已经通过产品可信度校验。
 */
typedef struct
{
    time_t   utc_timestamp; /**< SNTP 返回并写入 POSIX 时钟的 UTC Unix 秒 */
    uint32_t elapsed_ms;    /**< 本次初始化、等待、读取与清理总耗时 */
} time_sync_sample_t;

/**
 * @brief 串行执行一次 SNTP 网络取样并复制结果
 *
 * 本组件负责 ESP-NETIF SNTP 客户端的初始化、单样本等待与清理。ESP-IDF
 * 在收到样本时会更新 POSIX 墙上时钟；调用方仍必须独立校验候选范围、跳变和
 * 连续样本，并决定是否回写 RTC。函数不重试、不启动 Task，也不保存服务器地址。
 *
 * @param[in] server 非空 SNTP 服务器域名或地址
 * @param[in] timeout_ms 等待互斥取样和网络样本的各阶段超时，单位毫秒
 * @param[out] out_sample 样本副本，仅在 ESP_OK 时有效
 * @return ESP_OK 已取得样本；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_TIMEOUT
 *         等待取样槽或网络样本超时；或 ESP-NETIF、系统时钟错误码
 */
esp_err_t time_sync_sample_sntp_once_copy(const char *server, uint32_t timeout_ms, time_sync_sample_t *out_sample);

#ifdef __cplusplus
}
#endif
