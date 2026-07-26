/*
 * 文件职责：负责日历 JSON 数据解析和日程快照转换，不直接操作 UI 或 App 状态。
 * 主要依赖：dashboard_store、esp_event。
 * 调用方：app_network 和日历 Presenter。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

/** @brief 最大日程条目数 */
#define CALENDAR_EVENT_MAX 5

/**
 * @brief 日历服务事件类型
 */
typedef enum
{
    CALENDAR_DATA_EVENT_FAILED = 0, /*!< 获取失败 */
    CALENDAR_DATA_EVENT_REFRESHED,  /*!< 数据已刷新 */
} calendar_data_event_t;

ESP_EVENT_DECLARE_BASE(CALENDAR_DATA_EVENT);

/**
 * @brief 单条日程事件结构体
 */
typedef struct
{
    char title[48];    /*!< 事件标题 */
    char relative[24]; /*!< 本地化时间文本，如 "明天 19:00" */
    bool all_day;      /*!< 是否全天事件 */
    char location[32]; /*!< 事件地点 */
} calendar_event_t;

/**
 * @brief 日历服务快照结构体
 */
typedef struct
{
    bool             valid;                      /*!< 数据是否有效 */
    char             source[16];                 /*!< 数据来源，如 "icloud" / "mock" / "" */
    calendar_event_t events[CALENDAR_EVENT_MAX]; /*!< 日程事件数组 */
    uint8_t          event_count;                /*!< 实际日程数 */
} calendar_snapshot_t;

/**
 * @brief 初始化日历服务
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t calendar_init(void);

/**
 * @brief 从 Dashboard Store 同步刷新日历快照
 *
 * @return ESP_OK 刷新并发布事件成功；其他值表示数据读取或事件发布失败
 */
esp_err_t calendar_refresh_from_dashboard(void);

/**
 * @brief 获取日历快照
 *
 * @param[out] out 指向 calendar_snapshot_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t calendar_get_snapshot(calendar_snapshot_t *out);
