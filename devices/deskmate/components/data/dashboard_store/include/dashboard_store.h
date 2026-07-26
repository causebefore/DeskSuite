/*
 * 文件职责：解析并缓存 Dashboard schema 3 数据，供各领域数据服务读取。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief ISO 时间字符串最大长度 */
#define DASHBOARD_STORE_ISO_TIME_MAX 32
/** @brief 城市名称最大长度 */
#define DASHBOARD_STORE_CITY_MAX     32
/** @brief 数据来源最大长度 */
#define DASHBOARD_STORE_SOURCE_MAX   16
/** @brief 错误信息最大长度 */
#define DASHBOARD_STORE_ERROR_MAX    96
/** @brief 最大天气预报天数 */
#define DASHBOARD_STORE_DAILY_MAX    3
/** @brief 最大日程条目数 */
#define DASHBOARD_STORE_CALENDAR_MAX 5
/** @brief 最大邮件条目数 */
#define DASHBOARD_STORE_MAIL_MAX     5
/** @brief 最大限额条目数 */
#define DASHBOARD_STORE_QUOTA_MAX    4

/**
 * @brief Dashboard 天气每日预报结构体
 */
typedef struct
{
    char fx_date[16];    /*!< 预报日期 */
    char text_day[24];   /*!< 白天天气描述 */
    char text_night[24]; /*!< 夜间天气描述 */
    char icon_day[8];    /*!< 白天天气图标代码 */
    int  temp_min_c;     /*!< 最低温度（°C） */
    int  temp_max_c;     /*!< 最高温度（°C） */
    char sunrise[8];     /*!< 日出时间 HH:MM */
    char sunset[8];      /*!< 日落时间 HH:MM */
} dashboard_store_weather_daily_t;

/**
 * @brief Dashboard 天气数据结构体
 */
typedef struct
{
    bool                            valid;                                    /*!< 数据是否有效 */
    char                            source[DASHBOARD_STORE_SOURCE_MAX];       /*!< 数据来源 */
    char                            updated_at[DASHBOARD_STORE_ISO_TIME_MAX]; /*!< 更新时间 */
    char                            error[DASHBOARD_STORE_ERROR_MAX];         /*!< 错误信息 */
    char                            city[DASHBOARD_STORE_CITY_MAX];           /*!< 城市名称 */
    char                            text[24];                                 /*!< 天气描述 */
    char                            icon[8];                                  /*!< 天气图标代码 */
    int                             temp_c;                                   /*!< 当前温度（°C） */
    int                             feels_like_c;                             /*!< 体感温度（°C） */
    uint8_t                         humidity_percent;                         /*!< 湿度百分比 */
    char                            wind_dir[24];                             /*!< 风向 */
    char                            wind_scale[12];                           /*!< 风力等级 */
    int                             pressure_hpa;                             /*!< 气压 hPa */
    float                           precip_mm;                                /*!< 降水量 mm */
    int                             vis_km;                                   /*!< 能见度 km */
    dashboard_store_weather_daily_t daily[DASHBOARD_STORE_DAILY_MAX];         /*!< 每日预报数组 */
    uint8_t                         daily_count;                              /*!< 实际天数 */
    uint16_t                        aqi;                                      /*!< 空气质量指数 */
    char                            aqi_category[16];                         /*!< 空气质量等级 */
    char                            minutely_summary[64];                     /*!< 分钟级降水摘要 */
    char                            alert_title[64];                          /*!< 预警标题 */
    char                            alert_severity[16];                       /*!< 预警严重程度 */
} dashboard_store_weather_t;

/**
 * @brief Dashboard 日程事件结构体
 */
typedef struct
{
    char title[48];    /*!< 事件标题 */
    char relative[24]; /*!< 相对时间文本 */
    bool all_day;      /*!< 是否全天事件 */
    char location[32]; /*!< 事件地点 */
} dashboard_store_calendar_event_t;

/**
 * @brief Dashboard 日历数据结构体
 */
typedef struct
{
    bool                             valid;                                /*!< 数据是否有效 */
    char                             source[DASHBOARD_STORE_SOURCE_MAX];   /*!< 数据来源 */
    char                             error[DASHBOARD_STORE_ERROR_MAX];     /*!< 错误信息 */
    dashboard_store_calendar_event_t events[DASHBOARD_STORE_CALENDAR_MAX]; /*!< 日程事件数组 */
    uint8_t                          event_count;                          /*!< 实际日程数 */
} dashboard_store_calendar_t;

/**
 * @brief Dashboard 邮件消息结构体
 */
typedef struct
{
    char from_name[32]; /*!< 发件人姓名 */
    char subject[64];   /*!< 邮件主题 */
    char date_text[24]; /*!< 日期文本 */
    bool unread;        /*!< 是否未读 */
} dashboard_store_mail_message_t;

/**
 * @brief Dashboard 邮件数据结构体
 */
typedef struct
{
    bool                           valid;                              /*!< 数据是否有效 */
    char                           source[DASHBOARD_STORE_SOURCE_MAX]; /*!< 数据来源 */
    char                           error[DASHBOARD_STORE_ERROR_MAX];   /*!< 错误信息 */
    dashboard_store_mail_message_t messages[DASHBOARD_STORE_MAIL_MAX]; /*!< 邮件消息数组 */
    uint8_t                        message_count;                      /*!< 实际邮件数 */
    uint8_t                        unread_count;                       /*!< 未读邮件数 */
} dashboard_store_mail_t;

/**
 * @brief Dashboard 限额条目结构体
 */
typedef struct
{
    char  type[32];          /*!< 限额类型 */
    float used_percent;      /*!< 已使用百分比 */
    float remaining_percent; /*!< 剩余百分比 */
    char  next_reset[24];    /*!< 下次重置时间 */
} dashboard_store_quota_item_t;

/**
 * @brief Dashboard 限额数据结构体
 */
typedef struct
{
    bool                         valid;     /*!< dashboard 是否包含 quota 块（独立于 available） */
    bool                         available; /*!< 服务端查询是否成功 */
    char                         source[DASHBOARD_STORE_SOURCE_MAX];       /*!< 数据来源 */
    char                         level[16];                                /*!< 服务等级 */
    char                         error[DASHBOARD_STORE_ERROR_MAX];         /*!< 错误信息 */
    char                         updated_at[DASHBOARD_STORE_ISO_TIME_MAX]; /*!< 更新时间 */
    dashboard_store_quota_item_t limits[DASHBOARD_STORE_QUOTA_MAX];        /*!< 限额条目数组 */
    uint8_t                      limit_count;                              /*!< 实际限额条目数 */
} dashboard_store_quota_t;

/**
 * @brief Dashboard 快照结构体
 */
typedef struct
{
    bool                       valid;                                      /*!< 数据是否有效 */
    int                        schema;                                     /*!< 数据模式版本 */
    char                       device_id[32];                              /*!< 设备 ID */
    char                       generated_at[DASHBOARD_STORE_ISO_TIME_MAX]; /*!< 生成时间 */
    dashboard_store_weather_t  weather;                                    /*!< 天气数据 */
    dashboard_store_calendar_t calendar;                                   /*!< 日历数据 */
    dashboard_store_mail_t     mail;                                       /*!< 邮件数据 */
    dashboard_store_quota_t    quota;                                      /*!< 限额数据 */
} dashboard_store_snapshot_t;

/**
 * @brief 初始化 Dashboard 存储
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t dashboard_store_init(void);

/**
 * @brief 从 JSON 字符串更新 Dashboard 数据
 *
 * @param[in] json JSON 字符串
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t dashboard_store_update_from_json(const char *json);

/**
 * @brief 获取 Dashboard 快照
 *
 * @param[out] out 指向 dashboard_store_snapshot_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t dashboard_store_get_snapshot(dashboard_store_snapshot_t *out);

/**
 * @brief 获取 Dashboard 天气数据
 *
 * @param[out] out 指向 dashboard_store_weather_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t dashboard_store_get_weather(dashboard_store_weather_t *out);

/**
 * @brief 获取 Dashboard 日历数据
 *
 * @param[out] out 指向 dashboard_store_calendar_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t dashboard_store_get_calendar(dashboard_store_calendar_t *out);

/**
 * @brief 获取 Dashboard 邮件数据
 *
 * @param[out] out 指向 dashboard_store_mail_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t dashboard_store_get_mail(dashboard_store_mail_t *out);

/**
 * @brief 获取 Dashboard 限额数据
 *
 * @param[out] out 指向 dashboard_store_quota_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t dashboard_store_get_quota(dashboard_store_quota_t *out);
