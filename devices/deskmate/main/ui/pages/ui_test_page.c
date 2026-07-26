/*
 * 文件职责：实现开发阶段字体标本测试页，集中核验外置字库的显示效果。
 * 主要依赖：LVGL、ui_platform 字体接口、ui_common、ui_main。
 * 调用方：ui_router。
 */
#include "ui_test_page.h"

#include "ui_platform_font.h"
#include "ui_common.h"

/**
 * @brief 创建黑色文本标签并定位到指定坐标
 *
 * @param parent 父容器
 * @param text   标签文本
 * @param font   标签字体
 * @param x      左上角 x 坐标
 * @param y      左上角 y 坐标
 * @return 创建的标签对象指针
 */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, int32_t x, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

/**
 * @brief 在一行内并排放置字体名称标签与标本文字
 *
 * 名称标签固定使用 16 号常规字体，标本文字使用传入的 sample_font。
 *
 * @param parent       父容器
 * @param name         字体名称标签文本
 * @param sample       标本文字
 * @param sample_font  标本文字字体
 * @param y            行起始 y 坐标
 */
static void make_font_row(lv_obj_t *parent, const char *name, const char *sample, const lv_font_t *sample_font,
                          int32_t y)
{
    make_label(parent, name, ui_platform_font_get(16), 4, y);
    make_label(parent, sample, sample_font, 96, y);
}

esp_err_t ui_test_page_init(void)
{
    return ESP_OK;
}

esp_err_t ui_test_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_clean(parent);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    make_font_row(parent, "16 REG", "云端日程  ABC 123", ui_platform_font_get(16), 8);
    make_font_row(parent, "16 SEMI", "重点信息  ABC 123", ui_platform_font_get_semibold(16), 31);
    ui_common_new_hline(parent, 53);

    make_font_row(parent, "24 REG", "今日天气", ui_platform_font_get(24), 61);
    make_font_row(parent, "24 SEMI", "重要提醒", ui_platform_font_get_semibold(24), 91);
    ui_common_new_hline(parent, 120);

    make_font_row(parent, "32 REG", "精简清晰", ui_platform_font_get(32), 129);
    ui_common_new_hline(parent, 168);

    make_font_row(parent, "48 NUM", "08:42", ui_platform_font_get(48), 176);
    make_label(parent,
               ui_platform_font_get_status() == UI_PLATFORM_FONT_READY ? "6 / 6 字库已加载" : "字体已回退",
               ui_platform_font_get(16),
               4,
               244);

    return ESP_OK;
}

esp_err_t ui_test_page_update(lv_obj_t *parent)
{
    (void) parent;
    return ESP_OK;
}
