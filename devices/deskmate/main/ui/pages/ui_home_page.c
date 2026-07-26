/*
 * 文件职责：实现主页（时钟英雄 + 日程/待办 + 天气/室内）。
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
static lv_obj_t *s_weather_icon; /* 天气图标 */
static lv_obj_t *s_weather_temp; /* 天气温度（text24） */
static lv_obj_t *s_weather_text; /* 天气描述 */
static lv_obj_t *s_weather_meta; /* 体感 */
static lv_obj_t *s_indoor_temp;  /* 室内温度（text24） */
static lv_obj_t *s_indoor_humi;  /* 室内湿度（text24） */
static bool      s_created = false;

/**
 * @brief 绘制主页带2 的单栏（日程或待办）
 *
 * line2 为 NULL 时只画标题加一行条目；否则画标题加两行条目。
 *
 * @param parent 父容器
 * @param title  栏标题
 * @param line1  第一行条目文本
 * @param line2  第二行条目文本，可为 NULL
 * @param x      栏左上角 X 坐标
 * @param y      栏左上角 Y 坐标
 */
static void draw_agenda_column(lv_obj_t *parent, const char *title, const char *line1, const char *line2, int32_t x,
                               int32_t y)
{
    lv_obj_t *t = ui_common_new_text16(parent);
    ui_common_set_label(t, title, x, y, 184, 18, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *l1 = ui_common_new_text16(parent);
    ui_common_set_label(l1, line1, x, y + 22, 184, 18, LV_TEXT_ALIGN_LEFT);

    if (line2 != NULL)
    {
        lv_obj_t *l2 = ui_common_new_text16(parent);
        ui_common_set_label(l2, line2, x, y + 40, 184, 18, LV_TEXT_ALIGN_LEFT);
    }
}

/**
 * @brief 一次性创建主页全部控件并设置位置、尺寸和样式
 *
 * 创建带1 时钟英雄、带2 日程/待办分栏、带3 天气/室内分栏，并标记 s_created 为 true。
 *
 * @param body 页面容器
 */
static void create_layout(lv_obj_t *body)
{
    /* ---- 带1: 时钟英雄 ---- */
    s_clock = ui_common_new_num48(body);
    ui_common_set_label(s_clock, "", 0, 8, UI_WIDTH, 56, LV_TEXT_ALIGN_CENTER);

    s_date = ui_common_new_text16(body);
    ui_common_set_label(s_date, "", 0, 66, UI_WIDTH, 20, LV_TEXT_ALIGN_CENTER);

    (void) ui_common_new_hline(body, 98);

    /* ---- 带2: 日程 | 待办 (竖线分栏) ---- */
    (void) ui_common_new_vline(body, 200, 106, 64);
    draw_agenda_column(body, "日程", "暂无日程", NULL, 12, 106);
    draw_agenda_column(body, "待办", "暂无提醒", NULL, 204, 106);

    (void) ui_common_new_hline(body, 178);

    /* ---- 带3: 天气 | 室内 (竖线分栏) ---- */
    (void) ui_common_new_vline(body, 232, 186, 80);

    /* 天气图标：创建后隐藏，populate 按数据 show/hide */
    s_weather_icon = lv_image_create(body);
    lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);

    s_weather_temp = ui_common_new_text24(body);
    ui_common_set_label(s_weather_temp, "--", 40, 190, 76, 26, LV_TEXT_ALIGN_LEFT);

    s_weather_text = ui_common_new_text16(body);
    ui_common_set_label(s_weather_text, "--", 120, 196, 108, 18, LV_TEXT_ALIGN_LEFT);

    s_weather_meta = ui_common_new_text16(body);
    ui_common_set_label(s_weather_meta, "体感 --", 40, 220, 188, 18, LV_TEXT_ALIGN_LEFT);

    /* ---- 室内 ---- */
    lv_obj_t *indoor_label = ui_common_new_text16(body);
    ui_common_set_label(indoor_label, "室内", 240, 190, 148, 18, LV_TEXT_ALIGN_LEFT);

    s_indoor_temp = ui_common_new_text24(body);
    ui_common_set_label(s_indoor_temp, "--.-\xc2\xb0", 240, 210, 80, 26, LV_TEXT_ALIGN_LEFT);

    s_indoor_humi = ui_common_new_text24(body);
    ui_common_set_label(s_indoor_humi, "--%", 322, 210, 66, 26, LV_TEXT_ALIGN_LEFT);

    s_created = true;
}

/**
 * @brief 按最新 view 更新主页动态文本和图标
 *
 * 只更新时钟、日期、天气与室内温湿度等动态字段，缺数据时填 "--"，
 * 不创建或销毁控件。
 *
 * @param view 主页视图切片
 */
static void populate(const home_view_model_t *view)
{
    char buf[32];

    /* ---- 时钟 + 日期 ---- */
    ui_format_time(&view->time, buf, sizeof(buf));
    lv_label_set_text(s_clock, buf);

    ui_format_home_date(&view->time, buf, sizeof(buf));
    lv_label_set_text(s_date, buf);

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
    }
    else
    {
        const int temp       = view->env.temperature_centi;
        const int temp_whole = temp / 100;
        const int temp_tenth = abs(temp % 100) / 10;
        snprintf(buf, sizeof(buf), "%d.%d\xc2\xb0", temp_whole, temp_tenth);
        lv_label_set_text(s_indoor_temp, buf);
    }

    snprintf(buf, sizeof(buf), "%u%%", (unsigned) (view->env.humidity_centi / 100));
    lv_label_set_text(s_indoor_humi, buf);
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
