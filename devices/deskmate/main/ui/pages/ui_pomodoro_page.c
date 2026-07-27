/**
 * @file ui_pomodoro_page.c
 * @brief 实现灰阶时间轨道番茄钟页面并按秒增量刷新
 */
#include "ui_pomodoro_page.h"

#include <stdio.h>
#include <string.h>

#include "pomodoro_presenter.h"
#include "ui_common.h"

#define TRACK_SEGMENT_COUNT 4U

static lv_obj_t *s_parent;
static lv_obj_t *s_phase_badge;
static lv_obj_t *s_phase_label;
static lv_obj_t *s_count_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_end_label;
static lv_obj_t *s_pause_badge;
static lv_obj_t *s_done_banner;
static lv_obj_t *s_done_title;
static lv_obj_t *s_done_detail;
static lv_obj_t *s_cycle_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_track_outer[TRACK_SEGMENT_COUNT];
static lv_obj_t *s_track_fill[TRACK_SEGMENT_COUNT];
static bool      s_created;

/** @brief 创建零圆角灰阶矩形 */
static lv_obj_t *new_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static void create_layout(lv_obj_t *parent)
{
    s_phase_badge = new_box(parent, 12, 16, 88, 28);
    lv_obj_set_style_bg_color(s_phase_badge, lv_color_black(), 0);
    s_phase_label = ui_common_new_inverse_text16_semibold(s_phase_badge);
    lv_obj_set_size(s_phase_label, 84, 20);
    lv_obj_center(s_phase_label);
    lv_obj_set_style_text_align(s_phase_label, LV_TEXT_ALIGN_CENTER, 0);

    s_count_label = ui_common_new_text16_regular(parent);
    ui_common_set_label(s_count_label, "", 244, 20, 144, 20, LV_TEXT_ALIGN_RIGHT);

    s_time_label = ui_common_new_num48(parent);
    ui_common_set_label(s_time_label, "", 0, 56, 400, 56, LV_TEXT_ALIGN_CENTER);

    s_end_label = ui_common_new_text16_regular(parent);
    ui_common_set_label(s_end_label, "", 0, 118, 400, 22, LV_TEXT_ALIGN_CENTER);

    s_pause_badge = new_box(parent, 156, 116, 88, 28);
    lv_obj_set_style_bg_color(s_pause_badge, lv_color_black(), 0);
    lv_obj_t *pause_text = ui_common_new_inverse_text16_semibold(s_pause_badge);
    lv_label_set_text(pause_text, "已暂停");
    lv_obj_center(pause_text);
    lv_obj_add_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);

    s_done_banner = new_box(parent, 0, 52, 400, 94);
    lv_obj_set_style_bg_color(s_done_banner, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_done_banner, 0, 0);
    s_done_title = ui_common_new_text24_semibold(s_done_banner);
    ui_common_set_label(s_done_title, "阶段完成", 12, 14, 376, 32, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_color(s_done_title, lv_color_white(), 0);
    s_done_detail = ui_common_new_text16_regular(s_done_banner);
    ui_common_set_label(s_done_detail, "", 12, 56, 376, 22, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_color(s_done_detail, lv_color_white(), 0);
    lv_obj_add_flag(s_done_banner, LV_OBJ_FLAG_HIDDEN);

    const int32_t gap   = 8;
    const int32_t width = (376 - gap * 3) / 4;
    for (uint32_t index = 0; index < TRACK_SEGMENT_COUNT; ++index)
    {
        const int32_t x      = 12 + (int32_t) index * (width + gap);
        s_track_outer[index] = new_box(parent, x, 158, width, 18);
        s_track_fill[index]  = lv_obj_create(s_track_outer[index]);
        lv_obj_remove_style_all(s_track_fill[index]);
        lv_obj_set_pos(s_track_fill[index], 1, 1);
        lv_obj_set_size(s_track_fill[index], 0, 16);
        lv_obj_set_style_bg_color(s_track_fill[index], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_track_fill[index], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_track_fill[index], LV_OBJ_FLAG_SCROLLABLE);
    }

    s_cycle_label = ui_common_new_text16_regular(parent);
    ui_common_set_label(s_cycle_label, "", 12, 184, 376, 22, LV_TEXT_ALIGN_LEFT);
    (void) ui_common_new_rule(parent, 12, 224, 376, UI_RULE_THIN);
    s_hint_label = ui_common_new_text16_semibold(parent);
    ui_common_set_label(s_hint_label, "", 12, 234, 376, 22, LV_TEXT_ALIGN_CENTER);
    s_created = true;
}

/** @brief 把四段轨道更新为整组进度和当前阶段进度 */
static void update_track(const pomodoro_view_model_t *view)
{
    const uint32_t interval       = view->long_break_interval > 0U ? view->long_break_interval : 4U;
    uint32_t       progress_units = ((uint32_t) view->completed_in_cycle * TRACK_SEGMENT_COUNT * 100U) / interval;
    uint32_t       phase_percent  = 0U;
    if (view->duration_seconds > 0U && view->run_state != POMODORO_VIEW_RUN_IDLE)
    {
        const uint32_t elapsed =
            view->duration_seconds > view->remaining_seconds ? view->duration_seconds - view->remaining_seconds : 0U;
        phase_percent = elapsed * 100U / view->duration_seconds;
    }
    if (view->run_state != POMODORO_VIEW_RUN_IDLE)
    {
        const uint32_t current_phase_units =
            (view->phase == POMODORO_VIEW_PHASE_FOCUS ? phase_percent : 35U) * TRACK_SEGMENT_COUNT / interval;
        progress_units += current_phase_units;
        if (progress_units > TRACK_SEGMENT_COUNT * 100U)
        {
            progress_units = TRACK_SEGMENT_COUNT * 100U;
        }
    }

    for (uint32_t index = 0; index < TRACK_SEGMENT_COUNT; ++index)
    {
        const uint32_t segment_start = index * 100U;
        const uint32_t fill_percent  = progress_units >= segment_start + 100U
                                           ? 100U
                                           : (progress_units > segment_start ? progress_units - segment_start : 0U);
        const int32_t  inner_width   = lv_obj_get_width(s_track_outer[index]) - 2;
        if (fill_percent == 0U)
        {
            lv_obj_add_flag(s_track_fill[index], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_set_width(s_track_fill[index], inner_width * (int32_t) fill_percent / 100);
            lv_obj_clear_flag(s_track_fill[index], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(s_track_fill[index],
                                  view->phase == POMODORO_VIEW_PHASE_FOCUS ? lv_color_black()
                                                                           : lv_palette_lighten(LV_PALETTE_GREY, 2),
                                  0);
    }
}

static void populate(const pomodoro_view_model_t *view)
{
    lv_label_set_text(s_phase_label, view->phase_text);
    lv_label_set_text(s_count_label, view->count_text);
    lv_label_set_text(s_time_label, view->time_text);
    lv_label_set_text(s_end_label, view->end_text);
    lv_label_set_text(s_cycle_label, view->cycle_text);
    lv_label_set_text(s_hint_label, view->hint_text);

    const bool paused = view->run_state == POMODORO_VIEW_RUN_PAUSED;
    const bool done   = view->run_state == POMODORO_VIEW_RUN_DONE;
    if (paused)
    {
        lv_obj_clear_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_end_label, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_end_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (done)
    {
        lv_label_set_text(s_done_title, view->phase == POMODORO_VIEW_PHASE_FOCUS ? "专注完成" : "休息结束");
        lv_label_set_text(s_done_detail, view->completion_text);
        lv_obj_clear_flag(s_done_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_end_label, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_done_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_time_label, LV_OBJ_FLAG_HIDDEN);
    }
    update_track(view);
}

esp_err_t ui_pomodoro_page_init(void)
{
    return ESP_OK;
}

void ui_pomodoro_page_deinit(void)
{
    s_parent      = NULL;
    s_phase_badge = NULL;
    s_phase_label = NULL;
    s_count_label = NULL;
    s_time_label  = NULL;
    s_end_label   = NULL;
    s_pause_badge = NULL;
    s_done_banner = NULL;
    s_done_title  = NULL;
    s_done_detail = NULL;
    s_cycle_label = NULL;
    s_hint_label  = NULL;
    memset(s_track_outer, 0, sizeof(s_track_outer));
    memset(s_track_fill, 0, sizeof(s_track_fill));
    s_created = false;
}

esp_err_t ui_pomodoro_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    ui_pomodoro_page_deinit();
    s_parent = parent;
    lv_obj_clean(parent);
    create_layout(parent);
    pomodoro_view_model_t view;
    const esp_err_t       error = pomodoro_presenter_get_view_copy(&view);
    if (error != ESP_OK)
    {
        return error;
    }
    populate(&view);
    return ESP_OK;
}

esp_err_t ui_pomodoro_page_update(lv_obj_t *parent)
{
    if (!s_created || parent == NULL || parent != s_parent)
    {
        return s_created ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    pomodoro_view_model_t view;
    const esp_err_t       error = pomodoro_presenter_get_view_copy(&view);
    if (error != ESP_OK)
    {
        return error;
    }
    populate(&view);
    return ESP_OK;
}
