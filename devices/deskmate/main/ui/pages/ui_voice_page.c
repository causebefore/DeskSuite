/*
 * 文件职责：实现语音交互页（居中大字状态 + 操作提示）。
 * 主要依赖：ui_common、voice_presenter。
 * 调用方：ui_router。
 */
#include "ui_voice_page.h"

#include "voice_presenter.h"
#include "ui_common.h"

/**
 * @brief 把语音状态枚举映射成居中主状态文案
 *
 * @param state 语音交互状态
 * @return 主状态文案字符串字面量（如"正在聆听…"/"语音助手"），调用方无需释放
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
 * @brief 把语音状态枚举映射成副提示文案
 *
 * @param state 语音交互状态
 * @return 副提示文案字符串字面量（如"松开右键结束录音"/"长按右键开始对话"），调用方无需释放
 */
static const char *state_hint_text(voice_view_state_t state)
{
    switch (state)
    {
        case VOICE_VIEW_STATE_RECORDING:
            return "松开右键结束录音";
        case VOICE_VIEW_STATE_THINKING:
            return "等待服务器响应";
        case VOICE_VIEW_STATE_SPEAKING:
            return "正在播放回复";
        case VOICE_VIEW_STATE_ERROR:
            return "长按右键重试";
        case VOICE_VIEW_STATE_IDLE:
        default:
            return "长按右键开始对话";
    }
}

/**
 * @brief 呼吸动画的执行回调：把动画值写入控件的整体不透明度
 *
 * @param obj    动画目标对象（lv_obj_t *）
 * @param value  当前动画帧的不透明度值（0..255）
 */
static void anim_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *) obj, (lv_opa_t) value, 0);
}

/**
 * @brief 按语音状态绘制整页
 *
 * 始终绘制居中主状态文案与副提示文案；活跃态额外绘制带呼吸动画的圆点，
 * 错误态额外绘制底部反白提示条，空闲态额外绘制底部三个小圆点装饰。
 *
 * @param body 页面容器
 * @param v    语音视图切片
 */
static void ui_voice_page_draw(lv_obj_t *body, const voice_view_model_t *v)
{
    const bool active   = v->busy;

    /* 居中主状态文字（num48 太大，用 text24 放大区域） */
    lv_obj_t *main_text = ui_common_new_text24(body);
    if (active)
    {
        lv_obj_set_style_text_color(main_text, lv_color_black(), 0);
    }
    ui_common_set_label(main_text, state_main_text(v->state), 0, 100, UI_WIDTH, 30, LV_TEXT_ALIGN_CENTER);

    /* 副提示文字 */
    lv_obj_t *hint = ui_common_new_text16(body);
    ui_common_set_label(hint, state_hint_text(v->state), 0, 140, UI_WIDTH, 20, LV_TEXT_ALIGN_CENTER);

    /* 活跃状态：呼吸动画 */
    if (active)
    {
        lv_obj_t *dot = lv_obj_create(body);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 170);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, dot);
        lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
        lv_anim_set_duration(&a, 800);
        lv_anim_set_playback_duration(&a, 800);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, anim_opa_cb);
        lv_anim_start(&a);
    }
    else if (v->state == VOICE_VIEW_STATE_ERROR)
    {
        /* 错误状态：底部反白提示条 */
        lv_obj_t *bar = ui_common_new_inverse_text16(body);
        ui_common_set_label(bar, state_hint_text(v->state), 60, 170, 280, 20, LV_TEXT_ALIGN_CENTER);
    }
    else
    {
        /* IDLE：底部小圆点装饰 */
        for (int i = 0; i < 3; ++i)
        {
            lv_obj_t *dot = lv_obj_create(body);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_pos(dot, UI_WIDTH / 2 - 20 + i * 14, 174);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
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

    voice_view_model_t view;
    voice_presenter_get_view_copy(&view);

    lv_obj_clean(parent);
    ui_voice_page_draw(parent, &view);
    return ESP_OK;
}

esp_err_t ui_voice_page_update(lv_obj_t *parent)
{
    return ui_voice_page_show(parent);
}
