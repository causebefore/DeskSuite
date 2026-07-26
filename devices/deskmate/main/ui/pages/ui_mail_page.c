/*
 * 文件职责：实现邮箱页（纯邮件列表，未读反白，无标题栏）。
 * 主要依赖：ui_common、mail_presenter。
 * 调用方：ui_router。
 */
#include "ui_mail_page.h"

#include "mail_presenter.h"
#include "ui_common.h"

#include <stdio.h>

/**
 * @brief 按邮箱视图绘制整页
 *
 * 有数据时逐条绘制邮件行（首行日期+发件人、次行主题），未读邮件用半粗字重和左侧黑条强调，
 * 行间以细线分隔；收件箱为空时居中显示"收件箱为空"；数据未到位时显示"正在加载邮件…"。
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
        const int32_t row_h   = 52;
        const int32_t list_y0 = 6;

        for (uint8_t i = 0; i < v->message_count && i < MAIL_VIEW_MESSAGE_MAX; ++i)
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
            ui_common_set_label(meta, buf, 10, y, 380, 18, LV_TEXT_ALIGN_LEFT);
            if (m->unread)
            {
                (void) ui_common_new_rule(body, 0, y + UI_SPACE_1, UI_SPACE_1, 12);
            }

            /* 第二行：主题。未读主题使用半粗 24px。 */
            lv_obj_t *subj = m->unread ? ui_common_new_text24_semibold(body) : ui_common_new_text24_regular(body);
            ui_common_set_label(subj, m->subject[0] ? m->subject : "(无主题)", 10, y + 24, 380, 26, LV_TEXT_ALIGN_LEFT);

            if (i + 1 < v->message_count)
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
