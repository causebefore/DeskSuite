/**
 * @file ui_pomodoro_banner.c
 * @brief 实现状态栏下方十秒全宽反白番茄钟完成提示
 */
#include "ui_pomodoro_banner.h"

#include "lvgl.h"
#include "pomodoro_presenter.h"
#include "ui_common.h"

#define POMODORO_BANNER_DURATION_MS 10000U

static lv_obj_t   *s_banner;
static lv_obj_t   *s_label;
static lv_timer_t *s_hide_timer;
static uint64_t    s_last_shown_generation;

static void hide_timer_callback(lv_timer_t *timer)
{
    if (s_banner != NULL)
    {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_pause(timer);
}

esp_err_t ui_pomodoro_banner_init(void)
{
    if (s_banner != NULL)
    {
        return ESP_OK;
    }
    s_banner = lv_obj_create(lv_layer_top());
    if (s_banner == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_pos(s_banner, 0, 32);
    lv_obj_set_size(s_banner, 400, 38);
    lv_obj_set_style_bg_color(s_banner, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    s_label = ui_common_new_inverse_text16_semibold(s_banner);
    ui_common_set_label(s_label, "", 8, 9, 384, 20, LV_TEXT_ALIGN_CENTER);
    s_hide_timer = lv_timer_create(hide_timer_callback, POMODORO_BANNER_DURATION_MS, NULL);
    if (s_hide_timer == NULL)
    {
        lv_obj_delete(s_banner);
        s_banner = NULL;
        s_label  = NULL;
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(s_hide_timer);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    return ESP_OK;
}

void ui_pomodoro_banner_deinit(void)
{
    if (s_hide_timer != NULL)
    {
        lv_timer_delete(s_hide_timer);
    }
    if (s_banner != NULL)
    {
        lv_obj_delete(s_banner);
    }
    s_banner                = NULL;
    s_label                 = NULL;
    s_hide_timer            = NULL;
    s_last_shown_generation = 0U;
}

esp_err_t ui_pomodoro_banner_sync(presentation_page_id_t current_page)
{
    if (s_banner == NULL || s_label == NULL || s_hide_timer == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    pomodoro_view_model_t view;
    const esp_err_t       error = pomodoro_presenter_get_view_copy(&view);
    if (error != ESP_OK)
    {
        return error;
    }
    if (!view.completion_latched || view.completion_generation == 0U)
    {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(s_hide_timer);
        return ESP_OK;
    }
    if (current_page == PRESENTATION_PAGE_POMODORO)
    {
        s_last_shown_generation = view.completion_generation;
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(s_hide_timer);
        return ESP_OK;
    }
    if (view.completion_generation == s_last_shown_generation)
    {
        return ESP_OK;
    }
    s_last_shown_generation = view.completion_generation;

    lv_label_set_text(s_label,
                      view.phase == POMODORO_VIEW_PHASE_FOCUS ? "专注完成，进入番茄钟确认"
                                                              : "休息结束，进入番茄钟确认");
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    lv_timer_set_period(s_hide_timer, POMODORO_BANNER_DURATION_MS);
    lv_timer_reset(s_hide_timer);
    lv_timer_resume(s_hide_timer);
    return ESP_OK;
}
