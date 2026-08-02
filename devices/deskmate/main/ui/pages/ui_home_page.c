/*
 * 文件职责：实现主页（时钟英雄 + 下一日程/未读邮件 + 天气/室内）。
 * 主要依赖：ui_common、ui_format、weather_icons、home_presenter。
 * 调用方：ui_router。
 *
 * 版式搬自参考项目 ui_home_page.c，适配当前字体与视图切片。
 * 天气块无论有没有数据都画（缺数据填 "--"），与系统页/天气页一致，不再用空态文案顶替。
 *
 * 增量刷新：_show 负责 clean + create_layout + populate；_update 只调 populate，
 * 不销毁不重建控件，只更新动态文本和图标。
 */
#include "ui_home_page.h"

#include "home_presenter.h"
#include "ui_common.h"
#include "ui_format.h"
#include "weather_icon_resolver.h"

#include <stdio.h>
#include <stdlib.h>

/* ── 动态控件句柄 ── */
static lv_obj_t *s_clock;        /* 时钟（num48） */
static lv_obj_t *s_date;         /* 日期 */
static lv_obj_t *s_next_event;   /* 下一日程标题 */
static lv_obj_t *s_next_meta;    /* 下一日程时间与地点 */
static lv_obj_t *s_unread_count; /* 未读邮件数 */
static lv_obj_t *s_unread_meta;  /* 未读邮件数据状态 */
static lv_obj_t *s_weather_icon; /* 天气图标 */
static lv_obj_t *s_weather_temp; /* 天气温度（text24） */
static lv_obj_t *s_weather_text; /* 天气描述 */
static lv_obj_t *s_weather_meta; /* 体感 */
static lv_obj_t *s_indoor_temp;  /* 室内温度（text24） */
static lv_obj_t *s_indoor_humi;  /* 室内湿度（text24） */
static bool      s_created = false;

/**
 * @brief 一次性创建主页全部控件并设置位置、尺寸和样式
 *
 * 创建带1 时钟英雄、带2 下一日程/未读邮件分栏、带3 天气/室内分栏，并标记 s_created 为 true。
 *
 * @param body 页面容器
 */
static void create_layout(lv_obj_t *body)
{
    /* ---- 带1: 时钟英雄 ---- */
    s_clock = ui_common_new_num48(body);
    ui_common_set_label(s_clock, "", 0, 8, UI_WIDTH, 56, LV_TEXT_ALIGN_CENTER);

    s_date = ui_common_new_text16_regular(body);
    ui_common_set_label(s_date, "", 0, 66, UI_WIDTH, 20, LV_TEXT_ALIGN_CENTER);

    (void) ui_common_new_rule(body, 0, 98, UI_WIDTH, UI_RULE_THIN);

    /* ---- 带2: 下一日程 | 未读邮件 ---- */
    (void) ui_common_new_rule(body, 280, 106, UI_RULE_THIN, 64);

    lv_obj_t *next_heading = ui_common_new_text16_semibold(body);
    ui_common_set_label(next_heading, "下一日程", 12, 106, 256, 18, LV_TEXT_ALIGN_LEFT);

    s_next_event = ui_common_new_text24_semibold(body);
    ui_common_set_label(s_next_event, "", 12, 126, 256, 28, LV_TEXT_ALIGN_LEFT);

    s_next_meta = ui_common_new_text16_regular(body);
    ui_common_set_label(s_next_meta, "", 12, 154, 256, 18, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *unread_heading = ui_common_new_text16_semibold(body);
    ui_common_set_label(unread_heading, "未读邮件", 292, 106, 96, 18, LV_TEXT_ALIGN_CENTER);

    s_unread_count = ui_common_new_text24_semibold(body);
    ui_common_set_label(s_unread_count, "", 292, 126, 96, 28, LV_TEXT_ALIGN_CENTER);

    s_unread_meta = ui_common_new_text16_regular(body);
    ui_common_set_label(s_unread_meta, "", 292, 154, 96, 18, LV_TEXT_ALIGN_CENTER);

    (void) ui_common_new_rule(body, 0, 178, UI_WIDTH, UI_RULE_THIN);

    /* ---- 带3: 天气 | 室内 (竖线分栏) ---- */
    (void) ui_common_new_rule(body, 232, 186, UI_RULE_THIN, 80);

    /* 天气图标：创建后隐藏，populate 按数据 show/hide */
    s_weather_icon = lv_image_create(body);
    lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);

    s_weather_temp = ui_common_new_text24_regular(body);
    ui_common_set_label(s_weather_temp, "--", 40, 190, 76, 26, LV_TEXT_ALIGN_LEFT);

    s_weather_text = ui_common_new_text16_regular(body);
    ui_common_set_label(s_weather_text, "--", 120, 196, 108, 18, LV_TEXT_ALIGN_LEFT);

    s_weather_meta = ui_common_new_text16_regular(body);
    ui_common_set_label(s_weather_meta, "体感 --", 40, 220, 188, 18, LV_TEXT_ALIGN_LEFT);

    /* ---- 室内 ---- */
    lv_obj_t *indoor_label = ui_common_new_text16_semibold(body);
    ui_common_set_label(indoor_label, "室内", 240, 190, 148, 18, LV_TEXT_ALIGN_LEFT);

    s_indoor_temp = ui_common_new_text24_regular(body);
    ui_common_set_label(s_indoor_temp, "--.-\xc2\xb0", 240, 210, 80, 26, LV_TEXT_ALIGN_LEFT);

    s_indoor_humi = ui_common_new_text24_regular(body);
    ui_common_set_label(s_indoor_humi, "--%", 322, 210, 66, 26, LV_TEXT_ALIGN_LEFT);

    s_created = true;
}

/**
 * @brief 按最新 view 更新主页动态文本和图标
 *
 * 只更新时钟、日期、下一日程、未读邮件、天气与室内温湿度，缺数据时显示明确状态，
 * 不创建或销毁控件。
 *
 * @param view 主页视图切片
 */
static void populate(const home_view_model_t *view)
{
    char buf[80];

    /* ---- 时钟 + 日期 ---- */
    ui_format_time(&view->time, buf, sizeof(buf));
    lv_label_set_text(s_clock, buf);

    ui_format_home_date(&view->time, buf, sizeof(buf));
    lv_label_set_text(s_date, buf);

    /* ---- 下一日程 + 未读邮件 ---- */
    const bool calendar_available =
        (view->next_event_status == PRESENTATION_DATA_OK || view->next_event_status == PRESENTATION_DATA_STALE);
    if (calendar_available && view->has_next_event)
    {
        lv_label_set_text(s_next_event, view->next_event.title[0] ? view->next_event.title : "(无标题)");
        if (view->next_event.relative[0] != '\0' && view->next_event.location[0] != '\0')
        {
            snprintf(buf, sizeof(buf), "%s · %s", view->next_event.relative, view->next_event.location);
        }
        else
        {
            snprintf(buf,
                     sizeof(buf),
                     "%s",
                     view->next_event.relative[0] ? view->next_event.relative : view->next_event.location);
        }
        lv_label_set_text(s_next_meta, buf);
        if (buf[0] != '\0')
        {
            lv_obj_clear_flag(s_next_meta, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_next_meta, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else
    {
        const char *next_text = calendar_available
                                    ? "暂无日程"
                                    : (view->next_event_status == PRESENTATION_DATA_EMPTY ? "等待同步" : "暂不可用");
        lv_label_set_text(s_next_event, next_text);
        lv_obj_add_flag(s_next_meta, LV_OBJ_FLAG_HIDDEN);
    }

    const bool mail_available =
        (view->unread_mail_status == PRESENTATION_DATA_OK || view->unread_mail_status == PRESENTATION_DATA_STALE);
    if (mail_available)
    {
        snprintf(buf, sizeof(buf), "%u 封", (unsigned) view->unread_mail_count);
        lv_label_set_text(s_unread_count, buf);
        if (view->unread_mail_status == PRESENTATION_DATA_STALE)
        {
            lv_label_set_text(s_unread_meta, "离线数据");
            lv_obj_clear_flag(s_unread_meta, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_unread_meta, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else
    {
        lv_label_set_text(s_unread_count, "--");
        lv_label_set_text(s_unread_meta, view->unread_mail_status == PRESENTATION_DATA_EMPTY ? "等待同步" : "暂不可用");
        lv_obj_clear_flag(s_unread_meta, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 天气 ---- */
    const bool ok = (view->weather.status == PRESENTATION_DATA_OK || view->weather.status == PRESENTATION_DATA_STALE);

    if (ok)
    {
        const lv_image_dsc_t *icon = weather_icon_resolver_get(view->weather.code, false);
        if (icon != NULL)
        {
            lv_image_set_src(s_weather_icon, icon);
            lv_obj_set_pos(s_weather_icon, 12, 196);
            lv_obj_clear_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else
    {
        lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
    }

    snprintf(buf, sizeof(buf), ok ? "%d\xc2\xb0" : "--", view->weather.temp_c);
    lv_label_set_text(s_weather_temp, buf);

    lv_label_set_text(s_weather_text, (ok && view->weather.text[0]) ? view->weather.text : "--");

    snprintf(buf,
             sizeof(buf),
             ok ? "\xe4\xbd\x93\xe6\x84\x9f%d\xc2\xb0" : "\xe4\xbd\x93\xe6\x84\x9f --",
             view->weather.feels_like_c);
    lv_label_set_text(s_weather_meta, buf);

    /* ---- 室内 ---- */
    if (view->env.status != PRESENTATION_DATA_OK)
    {
        lv_label_set_text(s_indoor_temp, "--.-\xc2\xb0");
        lv_label_set_text(s_indoor_humi, "--%");
    }
    else
    {
        const int temp       = view->env.temperature_centi;
        const int temp_whole = temp / 100;
        const int temp_tenth = abs(temp % 100) / 10;
        snprintf(buf, sizeof(buf), "%d.%d\xc2\xb0", temp_whole, temp_tenth);
        lv_label_set_text(s_indoor_temp, buf);

        snprintf(buf, sizeof(buf), "%u%%", (unsigned) (view->env.humidity_centi / 100));
        lv_label_set_text(s_indoor_humi, buf);
    }
}

esp_err_t ui_home_page_init(void)
{
    return ESP_OK;
}

/**
 * @brief 清空随主页控件树失效的动态控件句柄
 */
void ui_home_page_deinit(void)
{
    s_clock        = NULL;
    s_date         = NULL;
    s_next_event   = NULL;
    s_next_meta    = NULL;
    s_unread_count = NULL;
    s_unread_meta  = NULL;
    s_weather_icon = NULL;
    s_weather_temp = NULL;
    s_weather_text = NULL;
    s_weather_meta = NULL;
    s_indoor_temp  = NULL;
    s_indoor_humi  = NULL;
    s_created      = false;
}

esp_err_t ui_home_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_created = false;
    lv_obj_clean(parent);
    create_layout(parent);

    home_view_model_t view;
    home_presenter_get_view_copy(&view);
    populate(&view);
    return ESP_OK;
}

esp_err_t ui_home_page_update(lv_obj_t *parent)
{
    (void) parent;
    if (!s_created)
    {
        return ESP_ERR_INVALID_STATE;
    }

    home_view_model_t view;
    home_presenter_get_view_copy(&view);
    populate(&view);
    return ESP_OK;
}
