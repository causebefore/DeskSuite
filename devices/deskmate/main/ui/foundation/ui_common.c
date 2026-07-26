/*
 * 文件职责：实现 UI 共享样式与控件原语。
 * 主要依赖：ui_platform 字体接口、LVGL。
 * 调用方：各页面、ui_main。
 */
#include "ui_common.h"

#include "ui_platform_font.h"

#include <stdbool.h>
#include <string.h>

/**
 * @brief UI 共享样式与初始化状态的文件内私有容器
 *
 * ready 标记是否已通过 ui_common_init() 完成初始化；其余字段为各文本规格与
 * 卡片对应的 lv_style_t，由 ui_common_init() 填充、ui_common_deinit() 复位。
 */
typedef struct
{
    bool       ready;
    lv_style_t text_16_regular_style;
    lv_style_t text_16_semibold_style;
    lv_style_t text_24_regular_style;
    lv_style_t text_24_semibold_style;
    lv_style_t text_32_regular_style;
    lv_style_t text_num48_style;
    lv_style_t card_style;
} ui_common_view_t;

static ui_common_view_t s_view;

esp_err_t ui_common_init(void)
{
    if (s_view.ready)
    {
        return ESP_OK;
    }

    lv_style_init(&s_view.text_16_regular_style);
    lv_style_set_text_color(&s_view.text_16_regular_style, lv_color_black());
    lv_style_set_text_font(&s_view.text_16_regular_style, ui_platform_font_get(16));
    lv_style_set_pad_all(&s_view.text_16_regular_style, 0);

    lv_style_init(&s_view.text_16_semibold_style);
    lv_style_set_text_color(&s_view.text_16_semibold_style, lv_color_black());
    lv_style_set_text_font(&s_view.text_16_semibold_style, ui_platform_font_get_semibold(16));
    lv_style_set_pad_all(&s_view.text_16_semibold_style, 0);

    lv_style_init(&s_view.text_24_regular_style);
    lv_style_set_text_color(&s_view.text_24_regular_style, lv_color_black());
    lv_style_set_text_font(&s_view.text_24_regular_style, ui_platform_font_get(24));
    lv_style_set_pad_all(&s_view.text_24_regular_style, 0);

    lv_style_init(&s_view.text_24_semibold_style);
    lv_style_set_text_color(&s_view.text_24_semibold_style, lv_color_black());
    lv_style_set_text_font(&s_view.text_24_semibold_style, ui_platform_font_get_semibold(24));
    lv_style_set_pad_all(&s_view.text_24_semibold_style, 0);

    lv_style_init(&s_view.text_32_regular_style);
    lv_style_set_text_color(&s_view.text_32_regular_style, lv_color_black());
    lv_style_set_text_font(&s_view.text_32_regular_style, ui_platform_font_get(32));
    lv_style_set_pad_all(&s_view.text_32_regular_style, 0);

    lv_style_init(&s_view.text_num48_style);
    lv_style_set_text_color(&s_view.text_num48_style, lv_color_black());
    lv_style_set_text_font(&s_view.text_num48_style, ui_platform_font_get(48));
    lv_style_set_pad_all(&s_view.text_num48_style, 0);

    lv_style_init(&s_view.card_style);
    lv_style_set_bg_color(&s_view.card_style, lv_color_white());
    lv_style_set_border_color(&s_view.card_style, lv_color_black());
    lv_style_set_border_width(&s_view.card_style, 1);
    lv_style_set_radius(&s_view.card_style, 0);
    lv_style_set_pad_all(&s_view.card_style, UI_SPACE_2);

    s_view.ready = true;
    return ESP_OK;
}

/**
 * @brief 在所有引用者删除后释放共享样式并清空运行时状态
 */
void ui_common_deinit(void)
{
    if (!s_view.ready)
    {
        return;
    }

    lv_style_reset(&s_view.text_16_regular_style);
    lv_style_reset(&s_view.text_16_semibold_style);
    lv_style_reset(&s_view.text_24_regular_style);
    lv_style_reset(&s_view.text_24_semibold_style);
    lv_style_reset(&s_view.text_32_regular_style);
    lv_style_reset(&s_view.text_num48_style);
    lv_style_reset(&s_view.card_style);
    memset(&s_view, 0, sizeof(s_view));
}

/**
 * @brief 创建带样式并默认无背景无边框的文本标签控件
 *
 * 长文本超出尺寸时按裁剪显示（LV_LABEL_LONG_CLIP）。
 *
 * @param parent 父对象
 * @param style  已初始化的样式指针，添加到局部状态 0
 * @return lv_obj_t* 新建的标签对象
 */
static lv_obj_t *new_label(lv_obj_t *parent, const lv_style_t *style)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, style, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}

lv_obj_t *ui_common_new_text16_regular(lv_obj_t *parent)
{
    return new_label(parent, &s_view.text_16_regular_style);
}

lv_obj_t *ui_common_new_text16_semibold(lv_obj_t *parent)
{
    return new_label(parent, &s_view.text_16_semibold_style);
}

lv_obj_t *ui_common_new_text24_regular(lv_obj_t *parent)
{
    return new_label(parent, &s_view.text_24_regular_style);
}

lv_obj_t *ui_common_new_text24_semibold(lv_obj_t *parent)
{
    return new_label(parent, &s_view.text_24_semibold_style);
}

lv_obj_t *ui_common_new_text32_regular(lv_obj_t *parent)
{
    return new_label(parent, &s_view.text_32_regular_style);
}

lv_obj_t *ui_common_new_num48(lv_obj_t *parent)
{
    return new_label(parent, &s_view.text_num48_style);
}

lv_obj_t *ui_common_new_inverse_text16(lv_obj_t *parent)
{
    lv_obj_t *label = ui_common_new_text16_regular(parent);
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return label;
}

lv_obj_t *ui_common_new_inverse_text16_semibold(lv_obj_t *parent)
{
    lv_obj_t *label = ui_common_new_text16_semibold(parent);
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return label;
}

/* 兼容层：全量迁移完成后删除 ui_common.h 中标记为旧 API 的声明与本实现。 */
lv_obj_t *ui_common_new_text16(lv_obj_t *parent)
{
    return ui_common_new_text16_regular(parent);
}

lv_obj_t *ui_common_new_text24(lv_obj_t *parent)
{
    return ui_common_new_text24_regular(parent);
}

lv_obj_t *ui_common_new_rule(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_size(rule, w, h);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    return rule;
}

lv_obj_t *ui_common_new_hline(lv_obj_t *parent, int32_t y)
{
    return ui_common_new_rule(parent, 0, y, UI_WIDTH, UI_RULE_THIN);
}

lv_obj_t *ui_common_new_vline(lv_obj_t *parent, int32_t x, int32_t y, int32_t h)
{
    return ui_common_new_rule(parent, x, y, UI_RULE_THIN, h);
}

lv_obj_t *ui_common_new_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_view.card_style, 0);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void ui_common_set_label(lv_obj_t *label, const char *text, int32_t x, int32_t y, int32_t w, int32_t h,
                         lv_text_align_t align)
{
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_text(label, text);
}

/* ── 动画原语 ── */

/**
 * @brief 透明度动画执行回调，将动画当前值写入控件局部透明度
 *
 * @param obj 目标控件（由动画框架以 void* 传入，内部转回 lv_obj_t*）
 * @param v   当前动画值（LV_OPA_TRANSP..LV_OPA_COVER）
 */
static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *) obj, (lv_opa_t) v, 0);
}

void ui_common_anim_fade_in(lv_obj_t *obj, uint32_t ms)
{
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
