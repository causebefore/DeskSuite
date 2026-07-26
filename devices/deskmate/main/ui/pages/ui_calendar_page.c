/*
 * 文件职责：实现日历页（纯日程列表，无标题栏）。
 * 主要依赖：ui_common、calendar_presenter。
 * 调用方：ui_router。
 */
#include "ui_calendar_page.h"

#include "calendar_presenter.h"
#include "ui_common.h"

#include <stdio.h>

/**
 * @brief 在指定 y 坐标绘制一条横跨页面宽度的 1px 黑色水平线
 *
 * @param parent 父容器
 * @param y      水平线顶端 y 坐标
 * @return 创建的水平线对象指针
 */
static lv_obj_t *draw_hline(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_size(bar, UI_WIDTH, 1);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

/**
 * @brief 按日历视图状态绘制日程列表
 *
 * @param body 页面内容区容器（调用前应为干净状态）
 * @param v    日历视图数据
 */
static void ui_calendar_page_draw(lv_obj_t *body, const calendar_view_model_t *v)
{
    const bool ok = (v->status == PRESENTATION_DATA_OK || v->status == PRESENTATION_DATA_STALE);

    if (ok && v->event_count > 0)
    {
        const int32_t row_h   = 50;
        const int32_t list_y0 = 6;

        for (uint8_t i = 0; i < v->event_count && i < CALENDAR_VIEW_EVENT_MAX; ++i)
        {
            const int32_t                      y = list_y0 + (int32_t) i * row_h;
            const calendar_event_view_model_t *e = &v->events[i];

            lv_obj_t *time_label                 = ui_common_new_text16(body);
            ui_common_set_label(time_label, e->relative[0] ? e->relative : "", 10, y, 380, 20, LV_TEXT_ALIGN_LEFT);

            lv_obj_t *title_label = ui_common_new_text24(body);
            ui_common_set_label(title_label,
                                e->title[0] ? e->title : "(无标题)",
                                10,
                                y + 20,
                                380,
                                26,
                                LV_TEXT_ALIGN_LEFT);

            if (i + 1 < v->event_count)
            {
                draw_hline(body, y + row_h - 2);
            }
        }
    }
    else if (ok)
    {
        lv_obj_t *empty = ui_common_new_text24(body);
        ui_common_set_label(empty, "未来 7 天没有日程", 10, 110, 380, 26, LV_TEXT_ALIGN_CENTER);
    }
    else
    {
        lv_obj_t *empty = ui_common_new_text16(body);
        ui_common_set_label(empty, "正在加载日程…", 10, 120, 380, 18, LV_TEXT_ALIGN_CENTER);
    }
}

esp_err_t ui_calendar_page_init(void)
{
    return ESP_OK;
}

esp_err_t ui_calendar_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    calendar_view_model_t view;
    calendar_presenter_get_view_copy(&view);

    lv_obj_clean(parent);
    ui_calendar_page_draw(parent, &view);
    return ESP_OK;
}

esp_err_t ui_calendar_page_update(lv_obj_t *parent)
{
    return ui_calendar_page_show(parent);
}
