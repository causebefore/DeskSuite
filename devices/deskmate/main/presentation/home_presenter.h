/*
 * 文件职责：聚合时间、天气、环境、下一日程和未读邮件事实，生成首页 View Model。
 */
#pragma once

#include "esp_err.h"
#include "presentation_view_model.h"

/** @brief 首页 View Model，聚合首页需要的有界呈现信息 */
typedef struct
{
    home_time_view_model_t        time;               /*!< 时间信息 */
    weather_view_model_t          weather;            /*!< 天气信息 */
    home_environment_view_model_t env;                /*!< 环境温湿度 */
    calendar_event_view_model_t   next_event;         /*!< 按日历列表排序得到的下一日程 */
    bool                          has_next_event;     /*!< 是否有可展示的下一日程 */
    presentation_data_status_t    next_event_status;  /*!< 下一日程数据状态；有效列表为空时仍为 OK */
    uint8_t                       unread_mail_count;  /*!< 收件箱完整未读邮件数 */
    presentation_data_status_t    unread_mail_status; /*!< 未读邮件数据状态 */
} home_view_model_t;

/**
 * @brief 初始化首页 Presenter
 *
 * 订阅时间和环境事实事件，初始化内部 View Model。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t home_presenter_init(void);

/**
 * @brief 复制首页当前 View Model
 *
 * @param[out] out_view 接收首页 View Model
 */
void home_presenter_get_view_copy(home_view_model_t *out_view);
