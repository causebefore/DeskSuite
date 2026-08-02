/*
 * 文件职责：定义 Presentation 提供给 UI 的只读 View Model。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief Wi-Fi SSID 最大长度（含 '\0'） */
#define PRESENTATION_PORTAL_SSID_MAX     33
/** @brief Wi-Fi 密码最大长度（含 '\0'） */
#define PRESENTATION_PORTAL_PASSWORD_MAX 65
/** @brief 配网门户 URL 最大长度（含 '\0'） */
#define PRESENTATION_PORTAL_URL_MAX      32
/** @brief QR 码文本最大长度（含 '\0'） */
#define PRESENTATION_PORTAL_QR_TEXT_MAX  160

/** @brief 数据状态枚举 */
typedef enum
{
    PRESENTATION_DATA_EMPTY = 0, /*!< 尚未取得任何数据 */
    PRESENTATION_DATA_OK,        /*!< 已取得有效数据，业务集合允许为空 */
    PRESENTATION_DATA_STALE,     /*!< 上次有效数据已过期，但仍可显示 */
    PRESENTATION_DATA_ERROR,     /*!< 数据源获取失败，当前没有有效新数据 */
} presentation_data_status_t;

/** @brief 配网门户 View Model */
typedef struct
{
    bool active;                                     /*!< 是否处于配网模式 */
    char ssid[PRESENTATION_PORTAL_SSID_MAX];         /*!< 热点 SSID */
    char password[PRESENTATION_PORTAL_PASSWORD_MAX]; /*!< 热点密码 */
    char portal_url[PRESENTATION_PORTAL_URL_MAX];    /*!< 配网门户地址 */
    char wifi_qr[PRESENTATION_PORTAL_QR_TEXT_MAX];   /*!< Wi-Fi 配网 QR 文本 */
    char url_qr[PRESENTATION_PORTAL_QR_TEXT_MAX];    /*!< URL 配网 QR 文本 */
} settings_portal_view_model_t;

/** @brief 首页时间 View Model */
typedef struct
{
    uint16_t                   year;   /*!< 年份 */
    uint8_t                    month;  /*!< 月份 (1-12) */
    uint8_t                    day;    /*!< 日 (1-31) */
    uint8_t                    hour;   /*!< 时 (0-23) */
    uint8_t                    minute; /*!< 分 (0-59) */
    presentation_data_status_t status; /*!< 数据状态 */
} home_time_view_model_t;

/** @brief 首页环境 View Model */
typedef struct
{
    int16_t                    temperature_centi; /*!< 温度 (0.01°C 为单位) */
    uint16_t                   humidity_centi;    /*!< 湿度 (0.01% 为单位) */
    presentation_data_status_t status;            /*!< 数据状态 */
} home_environment_view_model_t;

/** @brief 天气每日预报 View Model */
typedef struct
{
    char     fx_date[16];  /*!< 预报日期 */
    char     text_day[24]; /*!< 白天天气描述 */
    uint16_t icon_day;     /*!< 白天天气图标 ID */
    int      temp_min_c;   /*!< 最低温度 (°C) */
    int      temp_max_c;   /*!< 最高温度 (°C) */
    char     sunrise[8];   /*!< 日出时间 */
    char     sunset[8];    /*!< 日落时间 */
} weather_daily_view_model_t;

/** @brief 天气页 View Model */
typedef struct
{
    char                       city[32];         /*!< 城市名称 */
    char                       text[24];         /*!< 当前天气描述 */
    uint16_t                   code;             /*!< 天气代码 */
    int                        temp_c;           /*!< 当前温度 (°C) */
    int                        feels_like_c;     /*!< 体感温度 (°C) */
    uint8_t                    humidity;         /*!< 湿度 (%) */
    char                       wind_scale[12];   /*!< 风力等级 */
    int                        pressure_hpa;     /*!< 气压 (hPa) */
    float                      precip_mm;        /*!< 降水量 (mm) */
    int                        vis_km;           /*!< 能见度 (km) */
    weather_daily_view_model_t daily[3];         /*!< 三日预报 */
    uint8_t                    daily_count;      /*!< 预报天数 */
    uint16_t                   aqi;              /*!< 空气质量指数 */
    char                       aqi_category[16]; /*!< 空气质量等级 */
    char                       alert_title[64];  /*!< 气象预警标题 */
    char                       updated_at[32];   /*!< 数据更新时间 */
    presentation_data_status_t status;           /*!< 数据状态 */
} weather_view_model_t;

/** @brief 系统信息 View Model */
typedef struct
{
    char                       version[24];        /*!< 固件版本 */
    char                       build_time[24];     /*!< 编译时间 */
    uint32_t                   uptime_sec;         /*!< 运行时长 (秒) */
    uint32_t                   sram_total_kb;      /*!< SRAM 总量 (KB) */
    uint32_t                   sram_free_kb;       /*!< SRAM 空闲 (KB) */
    uint8_t                    sram_used_percent;  /*!< SRAM 使用率 (%) */
    uint32_t                   psram_total_kb;     /*!< PSRAM 总量 (KB) */
    uint32_t                   psram_free_kb;      /*!< PSRAM 空闲 (KB) */
    uint8_t                    psram_used_percent; /*!< PSRAM 使用率 (%) */
    uint16_t                   cpu_mhz;            /*!< CPU 频率 (MHz) */
    presentation_data_status_t status;             /*!< 数据状态 */
} system_info_view_model_t;

/* ── 日程页 view ── */
/** @brief 日历 View Model 事件最大数量 */
#define CALENDAR_VIEW_EVENT_MAX 5

/** @brief 单个日历事件 View Model */
typedef struct
{
    char title[48];    /*!< 事件标题 */
    char relative[24]; /*!< 本地化时间文本，如 "明天 19:00" */
    bool all_day;      /*!< 是否全天事件 */
    char location[32]; /*!< 事件地点 */
} calendar_event_view_model_t;

/** @brief 日历页 View Model */
typedef struct
{
    calendar_event_view_model_t events[CALENDAR_VIEW_EVENT_MAX]; /*!< 事件列表 */
    uint8_t                     event_count;                     /*!< 事件数量 */
    char                        source[16];                      /*!< 数据来源："icloud" / "mock" / "" */
    presentation_data_status_t  status;                          /*!< 数据状态 */
} calendar_view_model_t;

/* ── 邮箱页 view ── */
/** @brief 邮箱 View Model 消息最大数量 */
#define MAIL_VIEW_MESSAGE_MAX 5

/** @brief 单封邮件摘要 View Model */
typedef struct
{
    char from_name[32]; /*!< 发件人姓名 */
    char subject[64];   /*!< 邮件主题 */
    char date_text[24]; /*!< 本地化日期文本，如 "07-04 04:24" */
    bool unread;        /*!< 是否未读 */
} mail_message_view_model_t;

/** @brief 邮箱页 View Model */
typedef struct
{
    mail_message_view_model_t  messages[MAIL_VIEW_MESSAGE_MAX]; /*!< 邮件列表 */
    uint8_t                    message_count;                   /*!< 邮件数量 */
    uint8_t                    unread_count;                    /*!< 未读数量 */
    char                       source[16];                      /*!< 数据来源："qq-imap" / "mock" / "" */
    presentation_data_status_t status;                          /*!< 数据状态 */
} mail_view_model_t;

/* ── 语音页 view ── */
/** @brief 语音页呈现状态 */
typedef enum
{
    VOICE_VIEW_STATE_IDLE = 0,  /*!< 空闲状态 */
    VOICE_VIEW_STATE_RECORDING, /*!< 录音中 */
    VOICE_VIEW_STATE_THINKING,  /*!< 思考中（等待 AI 响应） */
    VOICE_VIEW_STATE_SPEAKING,  /*!< 播报中 */
    VOICE_VIEW_STATE_ERROR,     /*!< 错误状态 */
} voice_view_state_t;

/** @brief 语音页 View Model */
typedef struct
{
    voice_view_state_t state; /*!< 当前语音状态 */
    bool               busy;  /*!< 是否繁忙（录音/思考/播报中） */
} voice_view_model_t;

/* ── 限额页 view ── */
/** @brief 限额 View Model 条目最大数量 */
#define QUOTA_VIEW_LIMIT_MAX 4

/** @brief 单个限额条目 View Model */
typedef struct
{
    char  type[32];          /*!< 限额类型名称 */
    float used_percent;      /*!< 已使用百分比 */
    float remaining_percent; /*!< 剩余百分比 */
    char  next_reset[24];    /*!< 下次重置时间 */
} quota_item_view_model_t;

/** @brief 限额页 View Model */
typedef struct
{
    quota_item_view_model_t    limits[QUOTA_VIEW_LIMIT_MAX]; /*!< 限额条目列表 */
    uint8_t                    limit_count;                  /*!< 条目数量 */
    char                       level[16];                    /*!< 账户等级 */
    char                       source[16];                   /*!< 数据来源 */
    char                       error[96];                    /*!< 错误信息 */
    char                       updated_at[32];               /*!< 数据更新时间 */
    presentation_data_status_t status;                       /*!< 数据状态 */
} quota_view_model_t;
