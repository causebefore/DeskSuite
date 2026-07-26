/*
 * 文件职责：实现天气页（站点报告风格：状态带 + hero + 指标网格 + 三日预报）。
 * 主要依赖：ui_common、weather_icons、weather_presenter。
 * 调用方：ui_router。
 *
 * 版式搬自参考项目 ui_weather_page.c。无论有没有数据都画完整版式，缺数据字段填 "--"
 * （与系统页一致），不再用一句空态文案顶替整页。气压/降水/能见度与三日预报来自
 * weather 解析；空气质量/日出日落已接入，缺数据时填 "--"。
 */
#include "ui_weather_page.h"

#include "weather_presenter.h"
#include "ui_common.h"
#include "weather_icon_resolver.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* 动态文本控件引用。_create 一次性建布局，_populate 只改文本/可见性，
 * 数据刷新时跑马灯动画不被打断。 */
typedef struct
{
    lv_obj_t *city;
    lv_obj_t *warn;
    lv_obj_t *status;
    lv_obj_t *hero_icon;
    lv_obj_t *temp;
    lv_obj_t *cond;
    lv_obj_t *air;
    lv_obj_t *v_feels;
    lv_obj_t *v_humi;
    lv_obj_t *v_wind;
    lv_obj_t *v_press;
    lv_obj_t *v_precip;
    lv_obj_t *v_vis;
    lv_obj_t *daily_date[3];
    lv_obj_t *daily_icon[3];
    lv_obj_t *daily_text[3];
    lv_obj_t *daily_temp[3];
    lv_obj_t *sun;
    lv_obj_t *upd;
} weather_widgets_t;

static weather_widgets_t s_widgets;

/**
 * @brief 把数据状态枚举映射成电报式状态带的中文文案
 *
 * @param status 应用数据状态
 * @return 状态文案字符串字面量（在线/离线/等待/不可用），调用方无需释放
 */
static const char *weather_status_text(presentation_data_status_t status)
{
    switch (status)
    {
        case PRESENTATION_DATA_OK:
            return "在线";
        case PRESENTATION_DATA_STALE:
            return "离线";
        case PRESENTATION_DATA_EMPTY:
            return "等待";
        case PRESENTATION_DATA_ERROR:
        default:
            return "不可用";
    }
}

/**
 * @brief 把 UTC 日期时间分量换算成 epoch 秒
 *
 * 使用 Howard Hinnant 的 civil_from_days 算法，避免依赖 newlib 的 timegm 扩展。
 *
 * @param y  UTC 年份
 * @param mo UTC 月份（1-12）
 * @param d  UTC 日（1-31）
 * @param h  UTC 小时（0-23）
 * @param mi UTC 分钟（0-59）
 * @param s  UTC 秒（0-59）
 * @return 自 1970-01-01 00:00:00 UTC 起经过的秒数
 */
static time_t utc_to_epoch(int y, int mo, int d, int h, int mi, int s)
{
    y -= (mo <= 2);
    const int      era  = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe  = (unsigned) (y - era * 400);
    const unsigned doy  = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned) d - 1;
    const unsigned doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long     days = (long) era * 146097 + (long) doe - 719468;
    return (time_t) days * 86400 + (time_t) h * 3600 + (time_t) mi * 60 + (time_t) s;
}

/**
 * @brief 把服务端 updated_at 的 UTC ISO 串格式化为本地月日时分
 *
 * 输入形如 "2026-07-04T08:30:00Z"，按设备时区（system_clock 已 setenv TZ）换算后写入
 * "MM-DD HH:MM"；解析失败则写入 "--"。
 *
 * @param[in]  iso     UTC ISO 8601 时间串
 * @param[out] out     输出缓冲，写入格式化后的月日时分
 * @param      out_len 输出缓冲容量
 */
static void format_updated_mdhm(const char *iso, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0)
    {
        return;
    }
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
    {
        const time_t epoch = utc_to_epoch(y, mo, d, h, mi, s);
        struct tm    loc;
        if (localtime_r(&epoch, &loc) != NULL && strftime(out, out_len, "%m-%d %H:%M", &loc) > 0)
        {
            return;
        }
    }
    snprintf(out, out_len, "--");
}

/**
 * @brief 绘制实心黑色矩形条
 *
 * 兼用 2px 粗横向规则线与 1px 竖向分隔：填充黑色、无边框、不滚动。
 *
 * @param parent 父容器
 * @param x      左上角 X 坐标
 * @param y      左上角 Y 坐标
 * @param w      宽度
 * @param h      高度
 * @return 创建的 LVGL 对象指针，已挂到 parent 上
 */
static lv_obj_t *draw_bar(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

/**
 * @brief 绘制指标格的 key/value 标签对
 *
 * key 左对齐，value 占位 "--" 右对齐。返回 value label 引用供后续 update 持有。
 *
 * @param parent 父容器
 * @param key    指标名称（左对齐文本）
 * @param x      指标格左上角 X 坐标
 * @param y      指标格左上角 Y 坐标
 * @return value 标签的 LVGL 对象指针，初始文本为 "--"
 */
static lv_obj_t *draw_metric_pair(lv_obj_t *parent, const char *key, int32_t x, int32_t y)
{
    lv_obj_t *key_label = ui_common_new_text16(parent);
    ui_common_set_label(key_label, key, x, y, 34, 18, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *value_label = ui_common_new_text16(parent);
    ui_common_set_label(value_label, "--", x + 34, y, 48, 18, LV_TEXT_ALIGN_RIGHT);
    return value_label;
}

/**
 * @brief 一次性创建天气页完整布局
 *
 * 建状态带、左 hero、右 3×2 指标网格、三日预报与底部日出日落/更新时间，
 * 所有控件始终创建（缺数据时隐藏或填 "--"），并把引用记录到 s_widgets。
 *
 * @param body 页面容器
 */
static void ui_weather_page_create(lv_obj_t *body)
{
    memset(&s_widgets, 0, sizeof(s_widgets));

    /* ---- 电报式状态带 ---- */
    s_widgets.city = ui_common_new_text24(body);
    ui_common_set_label(s_widgets.city, "--", 6, 0, 54, 26, LV_TEXT_ALIGN_LEFT);

    /* 预警槽：居中于城市和状态之间，宽 272px 足以两行放下大多数预警文本。
     * 用 WRAP 自动换行 + CLIP 裁切超长部分，不再滚动——MIP 面板滚动占帧率且抖动。 */
    s_widgets.warn = ui_common_new_inverse_text16(body);
    ui_common_set_label(s_widgets.warn, "", 64, 0, 272, 28, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_widgets.warn, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_widgets.warn, LV_OBJ_FLAG_HIDDEN);

    /* 状态（始终反白，update 时按状态切换正/反白） */
    s_widgets.status = ui_common_new_inverse_text16(body);
    ui_common_set_label(s_widgets.status, "--", 340, 4, 56, 20, LV_TEXT_ALIGN_RIGHT);

    draw_bar(body, 0, 28, UI_WIDTH, 2);

    /* ---- 左 hero ---- */
    s_widgets.hero_icon = lv_image_create(body);
    lv_obj_set_pos(s_widgets.hero_icon, 20, 40);
    lv_obj_add_flag(s_widgets.hero_icon, LV_OBJ_FLAG_HIDDEN);

    s_widgets.temp = ui_common_new_num48(body);
    ui_common_set_label(s_widgets.temp, "--", 80, 34, 110, 54, LV_TEXT_ALIGN_LEFT);

    s_widgets.cond = ui_common_new_text24(body);
    ui_common_set_label(s_widgets.cond, "--", 84, 92, 110, 26, LV_TEXT_ALIGN_LEFT);

    s_widgets.air = ui_common_new_text16(body);
    ui_common_set_label(s_widgets.air, "空气 --", 84, 122, 110, 18, LV_TEXT_ALIGN_LEFT);

    draw_bar(body, 198, 34, 1, 106);

    /* ---- 右 3×2 指标网格 ---- */
    s_widgets.v_feels  = draw_metric_pair(body, "体感", 208, 44);
    s_widgets.v_humi   = draw_metric_pair(body, "湿度", 312, 44);
    s_widgets.v_wind   = draw_metric_pair(body, "风力", 208, 74);
    s_widgets.v_press  = draw_metric_pair(body, "气压", 312, 74);
    s_widgets.v_precip = draw_metric_pair(body, "降水", 208, 104);
    s_widgets.v_vis    = draw_metric_pair(body, "能见", 312, 104);

    /* ---- 三日预报 ---- */
    (void) ui_common_new_hline(body, 148);

    const int32_t col_w = UI_WIDTH / 3;
    for (int i = 0; i < 3; ++i)
    {
        const int32_t x         = i * col_w;
        s_widgets.daily_date[i] = ui_common_new_text16(body);
        ui_common_set_label(s_widgets.daily_date[i], "--", x, 156, col_w, 18, LV_TEXT_ALIGN_CENTER);

        s_widgets.daily_icon[i] = lv_image_create(body);
        lv_obj_set_pos(s_widgets.daily_icon[i], x + col_w / 2 - 10, 178);
        lv_obj_add_flag(s_widgets.daily_icon[i], LV_OBJ_FLAG_HIDDEN);

        s_widgets.daily_text[i] = ui_common_new_text16(body);
        ui_common_set_label(s_widgets.daily_text[i], "--", x, 202, col_w, 18, LV_TEXT_ALIGN_CENTER);

        s_widgets.daily_temp[i] = ui_common_new_text16(body);
        ui_common_set_label(s_widgets.daily_temp[i], "--", x, 222, col_w, 18, LV_TEXT_ALIGN_CENTER);
    }

    /* ---- 底部 ---- */
    s_widgets.sun = ui_common_new_text16(body);
    ui_common_set_label(s_widgets.sun, "", 10, 250, 200, 18, LV_TEXT_ALIGN_LEFT);
    lv_obj_add_flag(s_widgets.sun, LV_OBJ_FLAG_HIDDEN);

    s_widgets.upd = ui_common_new_text16(body);
    ui_common_set_label(s_widgets.upd, "", 240, 250, 150, 18, LV_TEXT_ALIGN_RIGHT);
    lv_obj_add_flag(s_widgets.upd, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 按最新 view 填充天气页文本与可见性
 *
 * 只更新已有控件的文本和图标显隐，不创建或销毁控件；缺数据字段填 "--" 或隐藏。
 *
 * @param w 天气视图切片
 */
static void ui_weather_page_populate(const weather_view_model_t *w)
{
    const bool ok = (w->status == PRESENTATION_DATA_OK || w->status == PRESENTATION_DATA_STALE);
    char       buf[64];

    /* 预警改为静态换行显示（不再滚动），天气页统一用 IDLE 级别。 */
    const bool has_alert = (ok && w->alert_title[0] != '\0');

    lv_label_set_text(s_widgets.city, (ok && w->city[0]) ? w->city : "--");

    if (has_alert)
    {
        lv_label_set_text(s_widgets.warn, w->alert_title);
        lv_obj_clear_flag(s_widgets.warn, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_widgets.warn, LV_OBJ_FLAG_HIDDEN);
    }

    /* OK 时状态用正常黑字白底，非 OK 用反白强调 */
    lv_label_set_text(s_widgets.status, weather_status_text(w->status));
    if (w->status == PRESENTATION_DATA_OK)
    {
        lv_obj_set_style_bg_opa(s_widgets.status, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_widgets.status, lv_color_black(), 0);
    }
    else
    {
        lv_obj_set_style_bg_opa(s_widgets.status, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_widgets.status, lv_color_white(), 0);
    }

    /* ---- 左 hero ---- */
    if (ok)
    {
        const lv_image_dsc_t *icon = weather_icon_resolver_get(w->code, true);
        if (icon)
        {
            lv_image_set_src(s_widgets.hero_icon, icon);
            lv_obj_clear_flag(s_widgets.hero_icon, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_widgets.hero_icon, LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(buf, sizeof(buf), "%d°", w->temp_c);
    }
    else
    {
        lv_obj_add_flag(s_widgets.hero_icon, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(s_widgets.temp, buf);
    lv_label_set_text(s_widgets.cond, (ok && w->text[0]) ? w->text : "--");

    if (ok && w->aqi > 0)
    {
        snprintf(buf, sizeof(buf), "空气 %u %s", (unsigned) w->aqi, w->aqi_category);
    }
    else
    {
        snprintf(buf, sizeof(buf), "空气 --");
    }
    lv_label_set_text(s_widgets.air, buf);

    /* ---- 右 3×2 指标网格 ---- */
    if (ok)
    {
        snprintf(buf, sizeof(buf), "%d°", w->feels_like_c);
        lv_label_set_text(s_widgets.v_feels, buf);
        snprintf(buf, sizeof(buf), "%u%%", (unsigned) w->humidity);
        lv_label_set_text(s_widgets.v_humi, buf);
        lv_label_set_text(s_widgets.v_wind, w->wind_scale[0] ? w->wind_scale : "--");
    }
    else
    {
        lv_label_set_text(s_widgets.v_feels, "--");
        lv_label_set_text(s_widgets.v_humi, "--");
        lv_label_set_text(s_widgets.v_wind, "--");
    }
    if (ok && w->pressure_hpa > 0)
    {
        snprintf(buf, sizeof(buf), "%d", w->pressure_hpa);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(s_widgets.v_press, buf);

    if (ok)
    {
        /* 降水 0.0mm 是有效值（无降水），仍显示。 */
        snprintf(buf, sizeof(buf), "%.1f", (double) w->precip_mm);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(s_widgets.v_precip, buf);

    if (ok && w->vis_km > 0)
    {
        snprintf(buf, sizeof(buf), "%d", w->vis_km);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(s_widgets.v_vis, buf);

    /* ---- 三日预报 ---- */
    const int32_t col_w        = UI_WIDTH / 3;
    const char   *day_names[3] = { "今天", "明天", "后天" };
    for (int i = 0; i < 3; ++i)
    {
        const int32_t x = i * col_w;
        if (ok && i < w->daily_count && w->daily[i].fx_date[0])
        {
            lv_label_set_text(s_widgets.daily_date[i], w->daily[i].fx_date);
        }
        else
        {
            lv_label_set_text(s_widgets.daily_date[i], day_names[i]);
        }

        if (ok && i < w->daily_count)
        {
            const weather_daily_view_model_t *d  = &w->daily[i];
            const lv_image_dsc_t             *di = weather_icon_resolver_get(d->icon_day, false);
            if (di)
            {
                lv_image_set_src(s_widgets.daily_icon[i], di);
                lv_obj_set_pos(s_widgets.daily_icon[i], x + col_w / 2 - 10, 178);
                lv_obj_clear_flag(s_widgets.daily_icon[i], LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_add_flag(s_widgets.daily_icon[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_label_set_text(s_widgets.daily_text[i], d->text_day[0] ? d->text_day : "--");
            snprintf(buf, sizeof(buf), "%d/%d°", d->temp_max_c, d->temp_min_c);
            lv_label_set_text(s_widgets.daily_temp[i], buf);
        }
        else
        {
            lv_obj_add_flag(s_widgets.daily_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_widgets.daily_text[i], "--");
            lv_label_set_text(s_widgets.daily_temp[i], "--");
        }
    }

    /* ---- 底部：左日出日落、右更新时间 ---- */
    if (ok && w->daily_count > 0 && (w->daily[0].sunrise[0] != '\0' || w->daily[0].sunset[0] != '\0'))
    {
        snprintf(buf,
                 sizeof(buf),
                 "日出 %s  日落 %s",
                 w->daily[0].sunrise[0] != '\0' ? w->daily[0].sunrise : "--",
                 w->daily[0].sunset[0] != '\0' ? w->daily[0].sunset : "--");
        lv_label_set_text(s_widgets.sun, buf);
        lv_obj_clear_flag(s_widgets.sun, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_widgets.sun, LV_OBJ_FLAG_HIDDEN);
    }

    if (ok && w->updated_at[0] != '\0')
    {
        char when[16];
        format_updated_mdhm(w->updated_at, when, sizeof(when));
        snprintf(buf, sizeof(buf), "更新 %s", when);
        lv_label_set_text(s_widgets.upd, buf);
        lv_obj_clear_flag(s_widgets.upd, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_widgets.upd, LV_OBJ_FLAG_HIDDEN);
    }
}

esp_err_t ui_weather_page_init(void)
{
    return ESP_OK;
}

void ui_weather_page_deinit(void)
{
    memset(&s_widgets, 0, sizeof(s_widgets));
}

esp_err_t ui_weather_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    weather_view_model_t view;
    weather_presenter_get_view_copy(&view);

    lv_obj_clean(parent);
    ui_weather_page_create(parent);
    ui_weather_page_populate(&view);
    return ESP_OK;
}

/* 数据刷新：只改文本不重建控件，跑马灯动画不被打断，不全屏变脏。 */
esp_err_t ui_weather_page_update(lv_obj_t *parent)
{
    if (parent == NULL || s_widgets.city == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    weather_view_model_t view;
    weather_presenter_get_view_copy(&view);

    ui_weather_page_populate(&view);
    return ESP_OK;
}
