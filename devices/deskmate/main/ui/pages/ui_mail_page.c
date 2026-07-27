/*
 * 文件职责：实现邮箱页（两封邮件摘要，未读优先，已读弱化）。
 * 主要依赖：ui_common、mail_presenter。
 * 调用方：ui_router。
 */
#include "ui_mail_page.h"

#include "mail_presenter.h"
#include "ui_common.h"

#include <stdio.h>

/** @brief 邮箱页同时显示的最大邮件数量 */
#define UI_MAIL_VISIBLE_MAX 2

/**
 * @brief 按邮箱视图绘制整页
 *
 * 有数据时显示未读总数，并绘制最多两封邮件（首行日期+发件人、后两行主题）。未读邮件用
 * 半粗字重和左侧黑条强调，已读邮件使用浅灰常规字重；收件箱为空时居中显示"收件箱为空"；
 * 数据未到位时显示"正在加载邮件…"。
 *
 * @param body 页面容器
 * @param v    邮箱视图切片
 */
static void ui_mail_page_draw(lv_obj_t *body, const mail_view_model_t *v)
{
    const bool ok = (v->status == PRESENTATION_DATA_OK || v->status == PRESENTATION_DATA_STALE);
    char       buf[80];

    if (ok && v->message_count > 0)
    {
        const uint8_t visible_count = v->message_count > UI_MAIL_VISIBLE_MAX ? UI_MAIL_VISIBLE_MAX : v->message_count;
        const int32_t row_h         = 116;
        const int32_t list_y0       = 28;

        snprintf(buf, sizeof(buf), "未读 %u 封", (unsigned) v->unread_count);
        lv_obj_t *summary = ui_common_new_text16_semibold(body);
        ui_common_set_label(summary, buf, 10, 4, 380, 18, LV_TEXT_ALIGN_LEFT);
        (void) ui_common_new_rule(body, 0, 25, UI_WIDTH, UI_RULE_THIN);

        for (uint8_t i = 0; i < visible_count; ++i)
        {
            const int32_t                    y = list_y0 + (int32_t) i * row_h;
            const mail_message_view_model_t *m = &v->messages[i];

            /* 第一行：日期 + 发件人。未读以左侧黑条和半粗字重强调。 */
            snprintf(buf,
                     sizeof(buf),
                     "%s  %s",
                     m->date_text[0] ? m->date_text : "",
                     m->from_name[0] ? m->from_name : "");
            lv_obj_t *meta = m->unread ? ui_common_new_text16_semibold(body) : ui_common_new_text16_regular(body);
            ui_common_set_label(meta, buf, 10, y + 8, 380, 18, LV_TEXT_ALIGN_LEFT);
            if (m->unread)
            {
                (void) ui_common_new_rule(body, 0, y + 10, UI_SPACE_1, 16);
            }
            else
            {
                lv_obj_set_style_text_color(meta, lv_color_hex(0x888888), 0);
            }

            /* 后两行：主题。未读使用半粗黑字，已读使用常规深灰字。 */
            lv_obj_t *subj = m->unread ? ui_common_new_text24_semibold(body) : ui_common_new_text24_regular(body);
            ui_common_set_label(subj, m->subject[0] ? m->subject : "(无主题)", 10, y + 36, 380, 58, LV_TEXT_ALIGN_LEFT);
            lv_label_set_long_mode(subj, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_line_space(subj, UI_SPACE_1, 0);
            if (!m->unread)
            {
                lv_obj_set_style_text_color(subj, lv_color_hex(0x666666), 0);
            }

            if (i + 1 < visible_count)
            {
                (void) ui_common_new_rule(body, 0, y + row_h - UI_RULE_THIN, UI_WIDTH, UI_RULE_THIN);
            }
        }
    }
    else if (ok)
    {
        lv_obj_t *empty = ui_common_new_text24_semibold(body);
        ui_common_set_label(empty, "收件箱为空", 10, 110, 380, 26, LV_TEXT_ALIGN_CENTER);
    }
    else
    {
        lv_obj_t *empty = ui_common_new_text16_regular(body);
        ui_common_set_label(empty, "正在加载邮件…", 10, 120, 380, 18, LV_TEXT_ALIGN_CENTER);
    }
}

esp_err_t ui_mail_page_init(void)
{
    return ESP_OK;
}

esp_err_t ui_mail_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    mail_view_model_t view;
    mail_presenter_get_view_copy(&view);

    lv_obj_clean(parent);
    ui_mail_page_draw(parent, &view);
    return ESP_OK;
}

esp_err_t ui_mail_page_update(lv_obj_t *parent)
{
    return ui_mail_page_show(parent);
}
