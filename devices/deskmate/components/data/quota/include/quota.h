/*
 * 文件职责：从 dashboard_store 读取限额数据，维护限额快照。
 * 主要依赖：dashboard_store、esp_event。
 * 调用方：app_network 和限额 Presenter。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

/** @brief 最大限额条目数 */
#define QUOTA_LIMIT_MAX 4

/**
 * @brief 限额服务事件类型
 */
typedef enum
{
    QUOTA_EVENT_FAILED = 0, /*!< 获取失败 */
    QUOTA_EVENT_REFRESHED,  /*!< 数据已刷新 */
} quota_event_t;

ESP_EVENT_DECLARE_BASE(QUOTA_EVENT);

/**
 * @brief 单条限额条目结构体
 */
typedef struct
{
    char  type[32];          /*!< 限额类型 */
    float used_percent;      /*!< 已使用百分比 */
    float remaining_percent; /*!< 剩余百分比 */
    char  next_reset[24];    /*!< 下次重置时间 */
} quota_limit_t;

/**
 * @brief 限额服务快照结构体
 */
typedef struct
{
    bool          valid;                   /*!< 数据是否有效 */
    bool          available;               /*!< 服务是否可用 */
    char          source[16];              /*!< 数据来源 */
    char          level[16];               /*!< 服务等级 */
    char          error[96];               /*!< 错误信息 */
    char          updated_at[32];          /*!< 更新时间 */
    quota_limit_t limits[QUOTA_LIMIT_MAX]; /*!< 限额条目数组 */
    uint8_t       limit_count;             /*!< 实际限额条目数 */
} quota_snapshot_t;

/**
 * @brief 初始化限额服务
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t quota_init(void);

/**
 * @brief 从 Dashboard Store 同步刷新限额快照
 *
 * @return ESP_OK 刷新并发布事件成功；其他值表示数据读取或事件发布失败
 */
esp_err_t quota_refresh_from_dashboard(void);

/**
 * @brief 获取限额快照
 *
 * @param[out] out 指向 quota_snapshot_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t quota_get_snapshot(quota_snapshot_t *out);
