/*
 * 文件职责：实现邮箱页（两封邮件摘要，未读优先）。
 * 主要依赖：ui_common、mail_presenter。
 * 调用方：ui_router。
 *
 * 增量刷新：_show 负责创建固定控件树，_update 只更新文本与显隐。
 */
#include "ui_mail_page.h"

#include "mail_presenter.h"
#include "ui_common.h"

#include <stdio.h>
#include <string.h>

/** @brief 邮箱页同时显示的最大邮件数量 */
#define UI_MAIL_VISIBLE_MAX 2
#define UI_MAIL_ROW_HEIGHT  116
#define UI_MAIL_LIST_Y      28

/** @brief 单封邮件摘要对应的固定控件句柄 */
typedef struct
{
    lv_obj_t *meta;
    lv_obj_t *subject;
    lv_obj_t *unread_mark;
    lv_obj_t *separator;
} mail_row_widgets_t;

/** @brief 邮箱页固定控件树的借用句柄 */
typedef struct
{
    lv_obj_t          *parent;
    lv_obj_t          *summary;
    lv_obj_t          *header_rule;
    mail_row_widgets_t rows[UI_MAIL_VISIBLE_MAX];
    lv_obj_t          *empty;
    lv_obj_t          *status;
} mail_page_widgets_t;

static mail_page_widgets_t s_widgets;

/** @brief 隐藏单行邮件的全部二值样式控件 */
static void hide_mail_row(mail_row_widgets_t *row)
{
    lv_obj_add_flag(row->meta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(row->subject, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(row->unread_mark, LV_OBJ_FLAG_HIDDEN);
    if (row->separator != NULL)
    {
        lv_obj_add_flag(row->separator, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 一次性创建邮箱页固定控件树
 *
 * 每行固定保留元数据、主题、未读实心标记与分隔线；不使用 I1 面板无法呈现的灰色或
 * 透明度。
 *
 * @param parent 页面内容区容器
 */
static void ui_mail_page_create(lv_obj_t *parent)
{
    memset(&s_widgets, 0, sizeof(s_widgets));
    s_widgets.parent  = parent;

    s_widgets.summary = ui_common_new_text16_semibold(parent);
    ui_common_set_label(s_widgets.summary, "", 10, 4, 380, 18, LV_TEXT_ALIGN_LEFT);
    lv_obj_add_flag(s_widgets.summary, LV_OBJ_FLAG_HIDDEN);

    s_widgets.header_rule = ui_common_new_rule(parent, 0, 25, UI_WIDTH, UI_RULE_THIN);
    lv_obj_add_flag(s_widgets.header_rule, LV_OBJ_FLAG_HIDDEN);

    for (uint8_t i = 0; i < UI_MAIL_VISIBLE_MAX; ++i)
    {
        const int32_t       y   = UI_MAIL_LIST_Y + (int32_t) i * UI_MAIL_ROW_HEIGHT;
        mail_row_widgets_t *row = &s_widgets.rows[i];

        row->meta               = ui_common_new_text16_regular(parent);
        ui_common_set_label(row->meta, "", 10, y + 8, 380, 18, LV_TEXT_ALIGN_LEFT);

        row->subject = ui_common_new_text24_semibold(parent);
        ui_common_set_label(row->subject, "", 10, y + 36, 380, 58, LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(row->subject, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(row->subject, UI_SPACE_1, 0);

        row->unread_mark = ui_common_new_rule(parent, 0, y + 10, UI_SPACE_1, 16);

        if (i + 1 < UI_MAIL_VISIBLE_MAX)
        {
            row->separator =
                ui_common_new_rule(parent, 0, y + UI_MAIL_ROW_HEIGHT - UI_RULE_THIN, UI_WIDTH, UI_RULE_THIN);
        }
        hide_mail_row(row);
    }

    s_widgets.empty = ui_common_new_text24_semibold(parent);
    ui_common_set_label(s_widgets.empty, "", 10, 110, 380, 26, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);

    s_widgets.status = ui_common_new_text16_regular(parent);
    ui_common_set_label(s_widgets.status, "", 10, 120, 380, 18, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_widgets.status, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 按最新 View Model 更新邮箱页文本和显隐
 *
 * OK/STALE 视为可显示快照；可显示但消息数为零时显示明确空态。主题统一使用半粗黑字，
 * 未读额外显示实心左标，完全遵守 I1 二值显示能力。
 *
 * @param view 邮箱视图数据
 */
static void ui_mail_page_populate(const mail_view_model_t *view)
{
    lv_obj_add_flag(s_widgets.summary, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.header_rule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.status, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < UI_MAIL_VISIBLE_MAX; ++i)
    {
        hide_mail_row(&s_widgets.rows[i]);
    }

    const bool available = (view->status == PRESENTATION_DATA_OK || view->status == PRESENTATION_DATA_STALE);
    if (available)
    {
        char summary[24];
        snprintf(summary, sizeof(summary), "未读 %u 封", (unsigned) view->unread_count);
        lv_label_set_text(s_widgets.summary, summary);
        lv_obj_clear_flag(s_widgets.summary, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_widgets.header_rule, LV_OBJ_FLAG_HIDDEN);
    }

    if (available && view->message_count > 0)
    {
        char          buf[80];
        const uint8_t visible_count =
            view->message_count > UI_MAIL_VISIBLE_MAX ? UI_MAIL_VISIBLE_MAX : view->message_count;
        for (uint8_t i = 0; i < visible_count; ++i)
        {
            const mail_message_view_model_t *message = &view->messages[i];
            mail_row_widgets_t              *row     = &s_widgets.rows[i];

            snprintf(buf,
                     sizeof(buf),
                     "%s  %s",
                     message->date_text[0] ? message->date_text : "",
                     message->from_name[0] ? message->from_name : "");
            lv_label_set_text(row->meta, buf);

            const char *subject = message->subject[0] ? message->subject : "(无主题)";
            lv_label_set_text(row->subject, subject);
            lv_obj_clear_flag(row->meta, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(row->subject, LV_OBJ_FLAG_HIDDEN);

            if (message->unread)
            {
                lv_obj_clear_flag(row->unread_mark, LV_OBJ_FLAG_HIDDEN);
            }

            if (i + 1 < visible_count && row->separator != NULL)
            {
                lv_obj_clear_flag(row->separator, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    if (available)
    {
        lv_label_set_text(s_widgets.empty, view->unread_count == 0 ? "收件箱为空" : "暂无邮件摘要");
        lv_obj_clear_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s_widgets.status, view->status == PRESENTATION_DATA_EMPTY ? "正在加载邮件…" : "邮件暂不可用");
    lv_obj_clear_flag(s_widgets.status, LV_OBJ_FLAG_HIDDEN);
}

esp_err_t ui_mail_page_init(void)
{
    memset(&s_widgets, 0, sizeof(s_widgets));
    return ESP_OK;
}

esp_err_t ui_mail_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_clean(parent);
    ui_mail_page_create(parent);

    mail_view_model_t view;
    mail_presenter_get_view_copy(&view);
    ui_mail_page_populate(&view);
    return ESP_OK;
}

esp_err_t ui_mail_page_update(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_widgets.parent != parent || s_widgets.status == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    mail_view_model_t view;
    mail_presenter_get_view_copy(&view);
    ui_mail_page_populate(&view);
    return ESP_OK;
}
