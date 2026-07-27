/*
 * 文件职责：定义 Application、Presentation 与 UI 共享的页面呈现契约。
 */
#pragma once

/**
 * @brief 页面过渡方向
 *
 * UI 根据该值选择页面过渡动画方向；Application 只表达导航意图。
 */
typedef enum
{
    PRESENTATION_NAV_DIR_NONE = 0, /*!< 无方向，直接切换 */
    PRESENTATION_NAV_DIR_FORWARD,  /*!< 前进方向 */
    PRESENTATION_NAV_DIR_BACKWARD, /*!< 后退方向 */
} presentation_nav_dir_t;

/** @brief 产品页面标识 */
typedef enum
{
    PRESENTATION_PAGE_HOME = 0, /*!< 首页 */
    PRESENTATION_PAGE_POMODORO, /*!< 番茄钟页 */
    PRESENTATION_PAGE_WEATHER,  /*!< 天气页 */
    PRESENTATION_PAGE_VOICE,    /*!< 语音交互页 */
    PRESENTATION_PAGE_CALENDAR, /*!< 日历页 */
    PRESENTATION_PAGE_MAIL,     /*!< 邮箱页 */
    PRESENTATION_PAGE_QUOTA,    /*!< 限额页 */
    PRESENTATION_PAGE_SETTINGS, /*!< 设置页 */
    PRESENTATION_PAGE_TEST,     /*!< 测试页 */
    PRESENTATION_PAGE_COUNT,    /*!< 页面总数（哨兵值） */
} presentation_page_id_t;
