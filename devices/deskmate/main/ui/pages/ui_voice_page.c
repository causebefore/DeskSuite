/*
 * 文件职责：实现语音交互页（居中大字状态 + 操作提示）。
 * 主要依赖：ui_common、voice_presenter。
 * 调用方：ui_router。
 *
 * 控件树只在页面首次显示时创建，状态刷新只修改文本与显隐。
 * I1 面板上只使用纯黑白标记，不使用透明度或淡入呼吸动画。
 */
#include "ui_voice_page.h"

#include "voice_presenter.h"
#include "ui_common.h"

#include <string.h>

/** @brief 语音页固定控件树的借用句柄。 */
typedef struct
{
    lv_obj_t *parent;
    lv_obj_t *main_text;
    lv_obj_t *hint;
    lv_obj_t *busy_marker;
    lv_obj_t *error_hint;
} voice_page_widgets_t;

static voice_page_widgets_t s_widgets;

/** @brief 页面容器删除时清空已失效的借用句柄。 */
static void on_parent_deleted(lv_event_t *event)
{
    if (lv_event_get_current_target(event) == s_widgets.parent)
    {
        memset(&s_widgets, 0, sizeof(s_widgets));
    }
}

/**
 * @brief 把语音状态枚举映射成居中主状态文案
 *
 * @param state 语音交互状态
 * @return 主状态文案字符串字面量
 */
static const char *state_main_text(voice_view_state_t state)
{
    switch (state)
    {
        case VOICE_VIEW_STATE_RECORDING:
            return "正在聆听…";
        case VOICE_VIEW_STATE_THINKING:
            return "思考中…";
        case VOICE_VIEW_STATE_SPEAKING:
            return "回复中";
        case VOICE_VIEW_STATE_ERROR:
            return "出错，请重试";
        case VOICE_VIEW_STATE_IDLE:
        default:
            return "语音助手";
    }
}

/**
 * @brief 把语音状态枚举映射成右键操作提示
 *
 * 开始后右键长按取消整个对话，不把录音误表述为“松开结束”。
 *
 * @param state 语音交互状态
 * @return 操作提示字符串字面量
 */
static const char *state_hint_text(voice_view_state_t state)
{
    switch (state)
    {
        case VOICE_VIEW_STATE_RECORDING:
        case VOICE_VIEW_STATE_THINKING:
        case VOICE_VIEW_STATE_SPEAKING:
            return "长按右键取消对话";
        case VOICE_VIEW_STATE_ERROR:
            return "长按右键重新开始";
        case VOICE_VIEW_STATE_IDLE:
        default:
            return "长按右键开始对话";
    }
}

/** @brief 一次性创建主状态、操作提示与二值状态标记。 */
static void ui_voice_page_create(lv_obj_t *body)
{
    s_widgets.main_text = ui_common_new_text24_semibold(body);
    ui_common_set_label(s_widgets.main_text, "", 0, 100, UI_WIDTH, 30, LV_TEXT_ALIGN_CENTER);

    s_widgets.hint = ui_common_new_text16_regular(body);
    ui_common_set_label(s_widgets.hint, "", 0, 140, UI_WIDTH, 20, LV_TEXT_ALIGN_CENTER);

    s_widgets.busy_marker = ui_common_new_rule(body, UI_WIDTH / 2 - 24, 174, 48, UI_RULE_STRONG);
    lv_obj_add_flag(s_widgets.busy_marker, LV_OBJ_FLAG_HIDDEN);

    s_widgets.error_hint = ui_common_new_inverse_text16_semibold(body);
    ui_common_set_label(s_widgets.error_hint, "", 60, 170, 280, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_widgets.error_hint, LV_OBJ_FLAG_HIDDEN);
}

/** @brief 按最新语音状态更新已有控件的文本与显隐。 */
static void ui_voice_page_populate(const voice_view_model_t *v)
{
    const bool error = (v->state == VOICE_VIEW_STATE_ERROR);
    const bool busy  = (v->busy && !error);

    lv_label_set_text(s_widgets.main_text, state_main_text(v->state));
    lv_label_set_text(s_widgets.hint, state_hint_text(v->state));
    lv_label_set_text(s_widgets.error_hint, state_hint_text(v->state));

    if (error)
    {
        lv_obj_add_flag(s_widgets.hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_widgets.busy_marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_widgets.error_hint, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_clear_flag(s_widgets.hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_widgets.error_hint, LV_OBJ_FLAG_HIDDEN);
        if (busy)
        {
            lv_obj_clear_flag(s_widgets.busy_marker, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_widgets.busy_marker, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

esp_err_t ui_voice_page_init(void)
{
    return ESP_OK;
}

esp_err_t ui_voice_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const bool parent_callback_registered = (s_widgets.parent == parent);
    lv_obj_clean(parent);
    memset(&s_widgets, 0, sizeof(s_widgets));
    s_widgets.parent = parent;
    if (!parent_callback_registered)
    {
        lv_obj_add_event_cb(parent, on_parent_deleted, LV_EVENT_DELETE, NULL);
    }
    ui_voice_page_create(parent);

    voice_view_model_t view;
    voice_presenter_get_view_copy(&view);
    ui_voice_page_populate(&view);
    return ESP_OK;
}

esp_err_t ui_voice_page_update(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (parent != s_widgets.parent || s_widgets.main_text == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    voice_view_model_t view;
    voice_presenter_get_view_copy(&view);
    ui_voice_page_populate(&view);
    return ESP_OK;
}
