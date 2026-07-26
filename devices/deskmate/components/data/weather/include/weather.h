/*
 * 文件职责：从 dashboard JSON 解析天气 page，维护天气快照。
 * 主要依赖：dashboard_store、esp_event。
 * 调用方：app_network 和天气 Presenter。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

/**
 * @brief 天气服务事件类型
 */
typedef enum
{
    WEATHER_EVENT_FAILED = 0, /*!< 获取失败 */
    WEATHER_EVENT_REFRESHED,  /*!< 数据已刷新 */
} weather_event_t;

/**
 * @brief 单日预报切片结构体
 *
 * 对齐服务端 DailyForecastItem 的设备端子集。
 */
typedef struct
{
    char fx_date[16];    /*!< 预报日期 */
    char text_day[24];   /*!< 白天天气描述 */
    char text_night[24]; /*!< 夜间天气描述 */
    char icon_day[8];    /*!< 和风 icon code 字符串，供 UI 转 uint16_t 取图标 */
    int  temp_min_c;     /*!< 最低温度（°C） */
    int  temp_max_c;     /*!< 最高温度（°C） */
    char sunrise[8];     /*!< 日出 HH:MM，空串表示无数据 */
    char sunset[8];      /*!< 日落 HH:MM，空串表示无数据 */
} weather_daily_t;

/**
 * @brief 天气服务快照结构体
 */
typedef struct
{
    bool            valid;                /*!< 数据是否有效 */
    char            source[16];           /*!< 数据来源 */
    char            updated_at[32];       /*!< 更新时间 */
    char            city[32];             /*!< 城市名称 */
    char            weather_text[24];     /*!< 天气描述 */
    char            icon[8];              /*!< 天气图标代码 */
    int             temp_c;               /*!< 当前温度（°C） */
    int             feels_like_c;         /*!< 体感温度（°C） */
    uint8_t         humidity_percent;     /*!< 湿度百分比 */
    char            wind_dir[24];         /*!< 风向 */
    char            wind_scale[12];       /*!< 风力等级 */
    int             pressure_hpa;         /*!< 气压 hPa，0 视为无效 */
    float           precip_mm;            /*!< 当前降水量 mm */
    int             vis_km;               /*!< 能见度 km，0 视为无效 */
    weather_daily_t daily[3];             /*!< 服务端最多下发 3 天 */
    uint8_t         daily_count;          /*!< 实际天数 */
    uint16_t        aqi;                  /*!< 空气质量指数，0 视为无效（无数据或降级） */
    char            aqi_category[16];     /*!< 空气质量等级文字（如 "良"） */
    char            minutely_summary[64]; /*!< 分钟级降水摘要 */
    char            alert_title[64];      /*!< 预警标题 */
    char            alert_severity[16];   /*!< 预警严重程度 */
    char            error[96];            /*!< 错误信息 */
} weather_snapshot_t;

ESP_EVENT_DECLARE_BASE(WEATHER_EVENT);

/**
 * @brief 初始化天气服务
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t weather_init(void);

/**
 * @brief 从 Dashboard Store 同步刷新天气快照
 *
 * @return ESP_OK 刷新并发布事件成功；其他值表示数据读取或事件发布失败
 */
esp_err_t weather_refresh_from_dashboard(void);

/**
 * @brief 获取天气快照
 *
 * @param[out] out 指向 weather_snapshot_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t weather_get_snapshot(weather_snapshot_t *out);
