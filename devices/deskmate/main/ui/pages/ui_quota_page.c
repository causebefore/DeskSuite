/*
 * 文件职责：实现限额页（进度条阵列展示 GLM 各项用量）。
 * 主要依赖：ui_common、quota_presenter。
 * 调用方：ui_router。
 *
 * 三个 limit 的友好名映射：TIME_LIMIT → MCP 月额度；
 * 两个 TOKENS_LIMIT 按 next_reset 早晚区分（reset 早=每5小时额度，晚=每周额度）。
 * 控件树只在页面首次显示时创建，数据刷新只修改文本和显隐。
 */
#include "ui_quota_page.h"

#include "quota_presenter.h"
#include "ui_common.h"

#include <stdio.h>
#include <string.h>

#define UI_QUOTA_LIST_Y        4
#define UI_QUOTA_ROW_H         54
#define UI_QUOTA_BAR_X         10
#define UI_QUOTA_BAR_Y         31
#define UI_QUOTA_BAR_W         300
#define UI_QUOTA_BAR_H         10
#define UI_QUOTA_SEGMENT_COUNT 5
#define UI_QUOTA_SEGMENT_GAP   2
#define UI_QUOTA_FOOTER_RULE_Y 228

/** @brief 单条限额行的固定控件句柄。 */
typedef struct
{
    lv_obj_t *root;
    lv_obj_t *name;
    lv_obj_t *reset;
    lv_obj_t *segments[UI_QUOTA_SEGMENT_COUNT];
    lv_obj_t *percent;
    lv_obj_t *percent_alert;
    lv_obj_t *separator;
} quota_row_widgets_t;

/** @brief 限额页固定控件树的借用句柄。 */
typedef struct
{
    lv_obj_t           *parent;
    quota_row_widgets_t rows[QUOTA_VIEW_LIMIT_MAX];
    lv_obj_t           *empty;
    lv_obj_t           *loading;
    lv_obj_t           *error_title;
    lv_obj_t           *error_detail;
    lv_obj_t           *footer_rule;
    lv_obj_t           *updated_at;
} quota_page_widgets_t;

static quota_page_widgets_t s_widgets;

/** @brief 页面容器删除时清空已失效的借用句柄。 */
static void on_parent_deleted(lv_event_t *event)
{
    if (lv_event_get_current_target(event) == s_widgets.parent)
    {
        memset(&s_widgets, 0, sizeof(s_widgets));
    }
}

/**
 * @brief 取限额条目的友好显示名
 *
 * TIME_LIMIT 映射为"MCP 月额度"；TOKENS_LIMIT 按 short_tokens_idx 区分短周期
 * （每5小时额度）与长周期（每周额度）；其余类型回退为原始 type 或"--"。
 *
 * @param v                 限额视图
 * @param idx               当前限额条目索引
 * @param short_tokens_idx  TOKENS_LIMIT 中 next_reset 最早的索引
 * @return 友好名静态字符串指针
 */
static const char *quota_friendly_name(const quota_view_model_t *v, uint8_t idx, uint8_t short_tokens_idx)
{
    const char *type = v->limits[idx].type;
    if (strcmp(type, "TIME_LIMIT") == 0)
    {
        return "MCP 月额度";
    }
    if (strcmp(type, "TOKENS_LIMIT") == 0)
    {
        return (idx == short_tokens_idx) ? "每5小时额度" : "每周额度";
    }
    return type[0] ? type : "--";
}

/** @brief 创建单条限额行的固定控件树。 */
static void create_quota_row(lv_obj_t *parent, uint8_t index)
{
    quota_row_widgets_t *row = &s_widgets.rows[index];

    row->root                = lv_obj_create(parent);
    lv_obj_remove_style_all(row->root);
    lv_obj_set_pos(row->root, 0, UI_QUOTA_LIST_Y + (int32_t) index * UI_QUOTA_ROW_H);
    lv_obj_set_size(row->root, UI_WIDTH, UI_QUOTA_ROW_H);
    lv_obj_clear_flag(row->root, LV_OBJ_FLAG_SCROLLABLE);

    row->name = ui_common_new_text24_semibold(row->root);
    ui_common_set_label(row->name, "", UI_QUOTA_BAR_X, 0, 210, 26, LV_TEXT_ALIGN_LEFT);

    row->reset = ui_common_new_text16_regular(row->root);
    ui_common_set_label(row->reset, "", 224, 5, 166, 18, LV_TEXT_ALIGN_RIGHT);

    lv_obj_t *slot = lv_obj_create(row->root);
    lv_obj_set_pos(slot, UI_QUOTA_BAR_X, UI_QUOTA_BAR_Y);
    lv_obj_set_size(slot, UI_QUOTA_BAR_W, UI_QUOTA_BAR_H);
    lv_obj_set_style_bg_color(slot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(slot, lv_color_black(), 0);
    lv_obj_set_style_border_width(slot, UI_RULE_THIN, 0);
    lv_obj_set_style_radius(slot, 0, 0);
    lv_obj_set_style_pad_all(slot, 0, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t segment_width =
        (UI_QUOTA_BAR_W - 2 - (UI_QUOTA_SEGMENT_COUNT - 1) * UI_QUOTA_SEGMENT_GAP) / UI_QUOTA_SEGMENT_COUNT;
    for (uint8_t segment = 0; segment < UI_QUOTA_SEGMENT_COUNT; ++segment)
    {
        const int32_t x        = 1 + (int32_t) segment * (segment_width + UI_QUOTA_SEGMENT_GAP);
        row->segments[segment] = ui_common_new_rule(slot, x, 1, segment_width, UI_QUOTA_BAR_H - 2);
        lv_obj_add_flag(row->segments[segment], LV_OBJ_FLAG_HIDDEN);
    }

    row->percent = ui_common_new_text16_semibold(row->root);
    ui_common_set_label(row->percent, "", 316, 27, 74, 20, LV_TEXT_ALIGN_RIGHT);

    row->percent_alert = ui_common_new_inverse_text16_semibold(row->root);
    ui_common_set_label(row->percent_alert, "", 316, 27, 74, 20, LV_TEXT_ALIGN_RIGHT);
    lv_obj_add_flag(row->percent_alert, LV_OBJ_FLAG_HIDDEN);

    row->separator = ui_common_new_rule(row->root, 0, UI_QUOTA_ROW_H - UI_RULE_THIN, UI_WIDTH, UI_RULE_THIN);
    lv_obj_add_flag(row->separator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(row->root, LV_OBJ_FLAG_HIDDEN);
}

/** @brief 一次性创建限额列表、空态、错误态与更新时间控件。 */
static void ui_quota_page_create(lv_obj_t *body)
{
    for (uint8_t i = 0; i < QUOTA_VIEW_LIMIT_MAX; ++i)
    {
        create_quota_row(body, i);
    }

    s_widgets.empty = ui_common_new_text24_semibold(body);
    ui_common_set_label(s_widgets.empty, "暂无限额数据", 10, 110, 380, 26, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);

    s_widgets.loading = ui_common_new_text16_regular(body);
    ui_common_set_label(s_widgets.loading, "正在加载限额…", 10, 120, 380, 18, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_widgets.loading, LV_OBJ_FLAG_HIDDEN);

    s_widgets.error_title = ui_common_new_inverse_text16_semibold(body);
    ui_common_set_label(s_widgets.error_title, "限额查询失败", 60, 100, 280, 20, LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(s_widgets.error_title, LV_OBJ_FLAG_HIDDEN);

    s_widgets.error_detail = ui_common_new_text16_regular(body);
    ui_common_set_label(s_widgets.error_detail, "", 10, 126, 380, 40, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_widgets.error_detail, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_widgets.error_detail, LV_OBJ_FLAG_HIDDEN);

    s_widgets.footer_rule = ui_common_new_rule(body, 0, UI_QUOTA_FOOTER_RULE_Y, UI_WIDTH, UI_RULE_THIN);
    lv_obj_add_flag(s_widgets.footer_rule, LV_OBJ_FLAG_HIDDEN);

    s_widgets.updated_at = ui_common_new_text16_regular(body);
    ui_common_set_label(s_widgets.updated_at, "", 210, UI_QUOTA_FOOTER_RULE_Y + 8, 180, 18, LV_TEXT_ALIGN_RIGHT);
    lv_obj_add_flag(s_widgets.updated_at, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 把最新限额视图填入已有控件
 *
 * 有效空列表显示空态；只有 PRESENTATION_DATA_EMPTY 显示加载态。
 * 最多四行共用 216px 高度，始终避开底部更新时间。
 */
static void ui_quota_page_populate(const quota_view_model_t *v)
{
    const bool    available = (v->status == PRESENTATION_DATA_OK || v->status == PRESENTATION_DATA_STALE);
    const uint8_t count     = v->limit_count > QUOTA_VIEW_LIMIT_MAX ? QUOTA_VIEW_LIMIT_MAX : v->limit_count;
    char          buf[80];

    for (uint8_t i = 0; i < QUOTA_VIEW_LIMIT_MAX; ++i)
    {
        lv_obj_add_flag(s_widgets.rows[i].root, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.error_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.error_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.footer_rule, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_widgets.updated_at, LV_OBJ_FLAG_HIDDEN);

    if (available && count > 0)
    {
        uint8_t short_tokens_idx = UINT8_MAX;
        for (uint8_t i = 0; i < count; ++i)
        {
            if (strcmp(v->limits[i].type, "TOKENS_LIMIT") != 0)
            {
                continue;
            }
            if (short_tokens_idx == UINT8_MAX
                || strcmp(v->limits[i].next_reset, v->limits[short_tokens_idx].next_reset) < 0)
            {
                short_tokens_idx = i;
            }
        }

        for (uint8_t i = 0; i < count; ++i)
        {
            quota_row_widgets_t           *row = &s_widgets.rows[i];
            const quota_item_view_model_t *q   = &v->limits[i];

            lv_label_set_text(row->name, quota_friendly_name(v, i, short_tokens_idx));

            if (q->next_reset[0] != '\0')
            {
                const char *reset = strlen(q->next_reset) > 5 ? q->next_reset + 5 : q->next_reset;
                snprintf(buf, sizeof(buf), "重置 %.11s", reset);
                lv_label_set_text(row->reset, buf);
                lv_obj_clear_flag(row->reset, LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_add_flag(row->reset, LV_OBJ_FLAG_HIDDEN);
            }

            float percent = q->used_percent;
            if (percent < 0.0f)
            {
                percent = 0.0f;
            }
            else if (percent > 100.0f)
            {
                percent = 100.0f;
            }

            uint8_t visible_segments = (uint8_t) (percent / 20.0f);
            if ((float) visible_segments * 20.0f < percent)
            {
                visible_segments++;
            }
            for (uint8_t segment = 0; segment < UI_QUOTA_SEGMENT_COUNT; ++segment)
            {
                if (segment < visible_segments)
                {
                    lv_obj_clear_flag(row->segments[segment], LV_OBJ_FLAG_HIDDEN);
                }
                else
                {
                    lv_obj_add_flag(row->segments[segment], LV_OBJ_FLAG_HIDDEN);
                }
            }

            snprintf(buf, sizeof(buf), "%.0f%%", (double) percent);
            lv_label_set_text(row->percent, buf);
            lv_label_set_text(row->percent_alert, buf);
            if (percent > 80.0f)
            {
                lv_obj_add_flag(row->percent, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(row->percent_alert, LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_clear_flag(row->percent, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(row->percent_alert, LV_OBJ_FLAG_HIDDEN);
            }

            if (i + 1 < count)
            {
                lv_obj_clear_flag(row->separator, LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_add_flag(row->separator, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_clear_flag(row->root, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else if (available)
    {
        lv_obj_clear_flag(s_widgets.empty, LV_OBJ_FLAG_HIDDEN);
    }
    else if (v->status == PRESENTATION_DATA_ERROR)
    {
        lv_obj_clear_flag(s_widgets.error_title, LV_OBJ_FLAG_HIDDEN);
        if (v->error[0] != '\0')
        {
            lv_label_set_text(s_widgets.error_detail, v->error);
            lv_obj_clear_flag(s_widgets.error_detail, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else
    {
        lv_obj_clear_flag(s_widgets.loading, LV_OBJ_FLAG_HIDDEN);
    }

    if (available && v->updated_at[0] != '\0')
    {
        snprintf(buf, sizeof(buf), "更新 %.16s", v->updated_at);
        char *separator = strchr(buf, 'T');
        if (separator != NULL)
        {
            *separator = ' ';
        }
        lv_label_set_text(s_widgets.updated_at, buf);
        lv_obj_clear_flag(s_widgets.footer_rule, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_widgets.updated_at, LV_OBJ_FLAG_HIDDEN);
    }
}

esp_err_t ui_quota_page_init(void)
{
    return ESP_OK;
}

esp_err_t ui_quota_page_show(lv_obj_t *parent)
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
    ui_quota_page_create(parent);

    quota_view_model_t view;
    quota_presenter_get_view_copy(&view);
    ui_quota_page_populate(&view);
    return ESP_OK;
}

esp_err_t ui_quota_page_update(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (parent != s_widgets.parent || s_widgets.loading == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    quota_view_model_t view;
    quota_presenter_get_view_copy(&view);
    ui_quota_page_populate(&view);
    return ESP_OK;
}
