/**
 * @file deskmate_api.h
 * @brief 定义 DeskMate Dashboard HTTP 协议
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol_backend_context.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DESKMATE_API_DEVICE_ID_MAX          32
#define DESKMATE_API_ISO_TIME_MAX           32
#define DESKMATE_API_DASHBOARD_SCHEMA       3
#define DESKMATE_API_DASHBOARD_MAX_BYTES    12288U
#define DESKMATE_API_DASHBOARD_SOURCE_MAX   16
#define DESKMATE_API_DASHBOARD_ERROR_MAX    96
#define DESKMATE_API_DASHBOARD_CITY_MAX     32
#define DESKMATE_API_DASHBOARD_DAILY_MAX    3
#define DESKMATE_API_DASHBOARD_CALENDAR_MAX 5
#define DESKMATE_API_DASHBOARD_MAIL_MAX     5
#define DESKMATE_API_DASHBOARD_QUOTA_MAX    4

    /** @brief DeskMate API 同步请求配置 */
    typedef struct
    {
        const protocol_backend_context_t *backend;    /*!< 调用期间借用的完整后端上下文 */
        int                               timeout_ms; /*!< 单次 HTTP 请求超时 */
    } deskmate_api_client_t;

    /** @brief Dashboard 天气每日预报 */
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
    } deskmate_api_dashboard_weather_daily_t;

    /** @brief Dashboard 天气数据 */
    typedef struct
    {
        bool                                   valid;                                     /*!< 天气块是否通过结构校验 */
        char                                   source[DESKMATE_API_DASHBOARD_SOURCE_MAX]; /*!< 数据来源 */
        char                                   updated_at[DESKMATE_API_ISO_TIME_MAX];     /*!< 更新时间 */
        char                                   error[DESKMATE_API_DASHBOARD_ERROR_MAX];   /*!< 错误信息 */
        char                                   city[DESKMATE_API_DASHBOARD_CITY_MAX];     /*!< 城市名称 */
        char                                   text[24];                                  /*!< 天气描述 */
        char                                   icon[8];                                   /*!< 天气图标代码 */
        int                                    temp_c;                                    /*!< 当前温度（°C） */
        int                                    feels_like_c;                              /*!< 体感温度（°C） */
        uint8_t                                humidity_percent;                          /*!< 湿度百分比 */
        char                                   wind_dir[24];                              /*!< 风向 */
        char                                   wind_scale[12];                            /*!< 风力等级 */
        int                                    pressure_hpa;                              /*!< 气压 hPa */
        float                                  precip_mm;                                 /*!< 降水量 mm */
        int                                    vis_km;                                    /*!< 能见度 km */
        deskmate_api_dashboard_weather_daily_t daily[DESKMATE_API_DASHBOARD_DAILY_MAX];   /*!< 每日预报 */
        uint8_t                                daily_count;                               /*!< 实际天数 */
        uint16_t                               aqi;                                       /*!< 空气质量指数 */
        char                                   aqi_category[16];                          /*!< 空气质量等级 */
        char                                   minutely_summary[64];                      /*!< 分钟级降水摘要 */
        char                                   alert_title[64];                           /*!< 预警标题 */
        char                                   alert_severity[16];                        /*!< 预警严重程度 */
    } deskmate_api_dashboard_weather_t;

    /** @brief Dashboard 日程事件 */
    typedef struct
    {
        char title[48];    /*!< 事件标题 */
        char relative[24]; /*!< 相对时间文本 */
        bool all_day;      /*!< 是否全天事件 */
        char location[32]; /*!< 事件地点 */
    } deskmate_api_dashboard_calendar_event_t;

    /** @brief Dashboard 日历数据 */
    typedef struct
    {
        bool                                    valid; /*!< 日历块是否通过结构校验 */
        char                                    source[DESKMATE_API_DASHBOARD_SOURCE_MAX];   /*!< 数据来源 */
        char                                    error[DESKMATE_API_DASHBOARD_ERROR_MAX];     /*!< 错误信息 */
        deskmate_api_dashboard_calendar_event_t events[DESKMATE_API_DASHBOARD_CALENDAR_MAX]; /*!< 日程事件 */
        uint8_t                                 event_count;                                 /*!< 实际日程数 */
    } deskmate_api_dashboard_calendar_t;

    /** @brief Dashboard 邮件摘要 */
    typedef struct
    {
        char from_name[32]; /*!< 发件人姓名 */
        char subject[64];   /*!< 邮件主题 */
        char date_text[24]; /*!< 日期文本 */
        bool unread;        /*!< 是否未读 */
    } deskmate_api_dashboard_mail_message_t;

    /** @brief Dashboard 邮件数据 */
    typedef struct
    {
        bool                                  valid;                                     /*!< 邮件块是否通过结构校验 */
        char                                  source[DESKMATE_API_DASHBOARD_SOURCE_MAX]; /*!< 数据来源 */
        char                                  error[DESKMATE_API_DASHBOARD_ERROR_MAX];   /*!< 错误信息 */
        deskmate_api_dashboard_mail_message_t messages[DESKMATE_API_DASHBOARD_MAIL_MAX]; /*!< 邮件摘要 */
        uint8_t                               message_count;                             /*!< 实际邮件数 */
        uint8_t                               unread_count;                              /*!< 未读邮件数 */
    } deskmate_api_dashboard_mail_t;

    /** @brief Dashboard 限额条目 */
    typedef struct
    {
        char  type[32];          /*!< 限额类型 */
        float used_percent;      /*!< 已使用百分比 */
        float remaining_percent; /*!< 剩余百分比 */
        char  next_reset[24];    /*!< 下次重置时间 */
    } deskmate_api_dashboard_quota_item_t;

    /** @brief Dashboard 限额数据 */
    typedef struct
    {
        bool                                valid;                                     /*!< 限额块是否通过结构校验 */
        bool                                available;                                 /*!< 服务端查询是否成功 */
        char                                source[DESKMATE_API_DASHBOARD_SOURCE_MAX]; /*!< 数据来源 */
        char                                level[16];                                 /*!< 服务等级 */
        char                                error[DESKMATE_API_DASHBOARD_ERROR_MAX];   /*!< 错误信息 */
        char                                updated_at[DESKMATE_API_ISO_TIME_MAX];     /*!< 更新时间 */
        deskmate_api_dashboard_quota_item_t limits[DESKMATE_API_DASHBOARD_QUOTA_MAX];  /*!< 限额条目 */
        uint8_t                             limit_count;                               /*!< 实际限额条目数 */
    } deskmate_api_dashboard_quota_t;

    /** @brief 已完成身份和 schema 校验的完整 Dashboard 响应 */
    typedef struct
    {
        bool                              valid;                                   /*!< 完整契约是否通过校验 */
        int                               schema;                                  /*!< Dashboard schema 版本 */
        char                              device_id[DESKMATE_API_DEVICE_ID_MAX];   /*!< 服务端回显的设备 ID */
        char                              generated_at[DESKMATE_API_ISO_TIME_MAX]; /*!< 服务端生成时间 */
        int64_t                           next_refresh_at_utc;                     /*!< 下一次联网刷新 UTC 秒数 */
        deskmate_api_dashboard_weather_t  weather;                                 /*!< 天气数据 */
        deskmate_api_dashboard_calendar_t calendar;                                /*!< 日历数据 */
        deskmate_api_dashboard_mail_t     mail;                                    /*!< 邮件数据 */
        deskmate_api_dashboard_quota_t    quota;                                   /*!< 限额数据 */
    } deskmate_api_dashboard_result_t;

    /**
     * @brief 拉取并校验 DeskMate Dashboard schema 3
     *
     * @param[in] client 请求配置
     * @param[in] max_response_bytes 调用方响应体上限；高于协议上限时自动收紧为
     *                               DESKMATE_API_DASHBOARD_MAX_BYTES
     * @param[out] out 完整类型化 Dashboard 输出
     * @param[out] http_status 可选 HTTP 状态码输出
     * @return ESP_OK 成功；其他值表示参数、鉴权、传输或响应错误
     */
    esp_err_t deskmate_api_get_dashboard(const deskmate_api_client_t *client, size_t max_response_bytes,
                                         deskmate_api_dashboard_result_t *out, int *http_status);

#ifdef __cplusplus
}
#endif
