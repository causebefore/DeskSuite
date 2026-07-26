/*
 * 文件职责：负责邮箱 JSON 数据解析和邮件快照转换，不直接操作 UI 或 App 状态。
 * 主要依赖：dashboard_store、esp_event。
 * 调用方：app_network 和邮箱 Presenter。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

/** @brief 最大邮件条目数 */
#define MAIL_MESSAGE_MAX 5

/**
 * @brief 邮件服务事件类型
 */
typedef enum
{
    MAIL_EVENT_FAILED = 0, /*!< 获取失败 */
    MAIL_EVENT_REFRESHED,  /*!< 数据已刷新 */
} mail_event_t;

ESP_EVENT_DECLARE_BASE(MAIL_EVENT);

/**
 * @brief 单条邮件消息结构体
 */
typedef struct
{
    char from_name[32]; /*!< 发件人姓名 */
    char subject[64];   /*!< 邮件主题 */
    char date_text[24]; /*!< 本地化日期文本，如 "07-04 04:24" */
    bool unread;        /*!< 是否未读 */
} mail_message_t;

/**
 * @brief 邮件服务快照结构体
 */
typedef struct
{
    bool           valid;                      /*!< 数据是否有效 */
    char           source[16];                 /*!< 数据来源，如 "qq-imap" / "mock" / "" */
    mail_message_t messages[MAIL_MESSAGE_MAX]; /*!< 邮件消息数组 */
    uint8_t        message_count;              /*!< 实际邮件数 */
    uint8_t        unread_count;               /*!< 未读邮件数 */
} mail_snapshot_t;

/**
 * @brief 初始化邮件服务
 *
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t mail_init(void);

/**
 * @brief 从 Dashboard Store 同步刷新邮件快照
 *
 * @return ESP_OK 刷新并发布事件成功；其他值表示数据读取或事件发布失败
 */
esp_err_t mail_refresh_from_dashboard(void);

/**
 * @brief 获取邮件快照
 *
 * @param[out] out 指向 mail_snapshot_t 结构体的指针
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t mail_get_snapshot(mail_snapshot_t *out);
