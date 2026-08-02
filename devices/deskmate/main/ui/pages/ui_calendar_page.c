/*
 * 文件职责：实现日历页（纯日程列表，无标题栏）。
 * 主要依赖：ui_common、calendar_presenter。
 * 调用方：ui_router。
 *
 * 增量刷新：_show 负责创建固定控件树，_update 只更新文本与显隐。
 */
#include "ui_calendar_page.h"

#include "calendar_presenter.h"
#include "ui_common.h"

#include <string.h>

#define UI_CALENDAR_ROW_HEIGHT 50
#define UI_CALENDAR_LIST_Y     6

/** @brief 单行日程对应的固定控件句柄 */
typedef struct
{
    lv_obj_t *time;
    lv_obj_t *title;
    lv_obj_t *separator;
} calendar_row_widgets_t;

/** @brief 日历页固定控件树的借用句柄 */
typedef struct
{
    lv_obj_t              *parent;
    calendar_row_widgets_t rows[CALENDAR_VIEW_EVENT_MAX];
    lv_obj_t              *empty;
    lv_obj_t              *status;
} calendar_page_widgets_t;

static calendar_page_widgets_t s_widgets;

/**
 * @brief 一次性创建日历页固定控件树
 *
 * 预先创建最大数量的日程行和空态控件，后续刷新仅修改文本与显隐，避免整页重建。
 *
 * @param parent 页面内容区容器
 */
static void ui_calendar_page_create(lv_obj_t *parent)
{
    memset(&s_widgets, 0, sizeof(s_widgets));
    s_widgets.parent = parent;

    for (uint8_t i = 0; i < CALENDAR_VIEW_EVENT_MAX; ++i)
    {
        const int32_t y        = UI_CALENDAR_LIST_Y + (int32_t) i * UI_CALENDAR_ROW_HEIGHT;

        s_widgets.rows[i].time = ui_common_new_text16_regular(parent);
        ui_common_set_label(s_widgets.rows[i].time, "", 10, y, 380, 20, LV_TEXT_ALIGN_LEFT);
        lv_obj_add_flag(s_widgets.rows[i].time, LV_OBJ_FLAG_HIDDEN);

        s_widgets.rows[i].title = ui_common_new_text24_semibold(parent);
        ui_common_set_label(s_widgets.rows[i].title, "", 10, y + 20, 380, 26, LV_TEXT_ALIGN_LEFT);
        lv_obj_add_flag(s_widgets.rows[i].title, LV_OBJ_FLAG_HIDDEN);

        if (i + 1 < CALENDAR_VIEW_EVENT_MAX)
        {
            s_widgets.rows[i].separator =
                ui_common_new_rule(parent, 0, y + UI_CALENDAR_ROW_HEIGHT - 2, UI_WIDTH, UI_RULE_THIN);
            lv_obj_add_flag(s_widgets.rows[i].separator, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_widgets.empty = ui_common_new_text24_semibold(parent);
    ui_common_set_label(s_widgets.empty, "", 10, 110, 380, 26, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);

    s_widgets.status = ui_common_new_text16_regular(parent);
    ui_common_set_label(s_widgets.status, "", 10, 120, 380, 18, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_widgets.status, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 按最新 View Model 更新日历页文本和显隐
 *
 * OK/STALE 视为可显示快照；可显示但事件数为零时显示明确空态。EMPTY 仅表示尚未取得
 * 数据，ERROR 显示不可用状态。
 *
 * @param view 日历视图数据
 */
static void ui_calendar_page_populate(const calendar_view_model_t *view)
{
    for (uint8_t i = 0; i < CALENDAR_VIEW_EVENT_MAX; ++i)
    {
        lv_obj_add_flag(s_widgets.rows[i].time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_widgets.rows[i].title, LV_OBJ_FLAG_HIDDEN);
        if (s_widgets.rows[i].separator != NULL)
        {
            lv_obj_add_flag(s_widgets.rows[i].separator, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_add_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.status, LV_OBJ_FLAG_HIDDEN);

    const bool available = (view->status == PRESENTATION_DATA_OK || view->status == PRESENTATION_DATA_STALE);
    if (available && view->event_count > 0)
    {
        const uint8_t visible_count =
            view->event_count > CALENDAR_VIEW_EVENT_MAX ? CALENDAR_VIEW_EVENT_MAX : view->event_count;
        for (uint8_t i = 0; i < visible_count; ++i)
        {
            const calendar_event_view_model_t *event = &view->events[i];
            lv_label_set_text(s_widgets.rows[i].time, event->relative[0] ? event->relative : "");
            lv_label_set_text(s_widgets.rows[i].title, event->title[0] ? event->title : "(无标题)");
            lv_obj_clear_flag(s_widgets.rows[i].time, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_widgets.rows[i].title, LV_OBJ_FLAG_HIDDEN);
            if (i + 1 < visible_count && s_widgets.rows[i].separator != NULL)
            {
                lv_obj_clear_flag(s_widgets.rows[i].separator, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    if (available)
    {
        lv_label_set_text(s_widgets.empty, "未来 7 天没有日程");
        lv_obj_clear_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_widgets.status, view->status == PRESENTATION_DATA_EMPTY ? "正在加载日程…" : "日程暂不可用");
    lv_obj_clear_flag(s_widgets.status, LV_OBJ_FLAG_HIDDEN);
}

esp_err_t ui_calendar_page_init(void)
{
    memset(&s_widgets, 0, sizeof(s_widgets));
    return ESP_OK;
}

esp_err_t ui_calendar_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_clean(parent);
    ui_calendar_page_create(parent);

    calendar_view_model_t view;
    calendar_presenter_get_view_copy(&view);
    ui_calendar_page_populate(&view);
    return ESP_OK;
}

esp_err_t ui_calendar_page_update(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_widgets.parent != parent || s_widgets.status == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    calendar_view_model_t view;
    calendar_presenter_get_view_copy(&view);
    ui_calendar_page_populate(&view);
    return ESP_OK;
}
