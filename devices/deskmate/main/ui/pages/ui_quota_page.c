/*
 * 文件职责：实现限额页（进度条阵列展示 GLM 各项用量）。
 * 主要依赖：ui_common、quota_presenter。
 * 调用方：ui_router。
 *
 * 三个 limit 的友好名映射：TIME_LIMIT → MCP 月额度；
 * 两个 TOKENS_LIMIT 按 next_reset 早晚区分（reset 早=每5小时额度，晚=每周额度）。
 * 进度条自建（1px 外框 + 实心黑条填充），与天气页 draw_bar 实心黑块风格一致。
 */
#include "ui_quota_page.h"

#include "quota_presenter.h"
#include "ui_common.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 绘制单色进度条（1px 外框槽位 + 实心黑条填充已用部分）
 *
 * @param parent 父容器，槽位与填充均挂在其上
 * @param x      槽位左上角 x 坐标
 * @param y      槽位左上角 y 坐标
 * @param w      槽位总宽度（含 1px 边框）
 * @param h      槽位总高度（含 1px 边框）
 * @param pct    填充百分比，函数内钳到 [0, 100]
 */
static void draw_progress_bar(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, float pct)
{
    if (pct < 0.0f)
    {
        pct = 0.0f;
    }
    if (pct > 100.0f)
    {
        pct = 100.0f;
    }

    /* 外框槽位（白底黑框） */
    lv_obj_t *slot = lv_obj_create(parent);
    lv_obj_set_pos(slot, x, y);
    lv_obj_set_size(slot, w, h);
    lv_obj_set_style_bg_color(slot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(slot, lv_color_black(), 0);
    lv_obj_set_style_border_width(slot, 1, 0);
    lv_obj_set_style_radius(slot, 0, 0);
    lv_obj_set_style_pad_all(slot, 0, 0);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

    /* 实心填充（钳到内框宽度，避免溢出 1px 边框） */
    int32_t fill_w  = (int32_t) ((float) w * pct / 100.0f);
    int32_t inner_w = w - 2;
    if (fill_w > inner_w)
    {
        fill_w = inner_w;
    }
    if (fill_w > 0)
    {
        lv_obj_t *fill = lv_obj_create(parent);
        lv_obj_set_pos(fill, x + 1, y + 1);
        lv_obj_set_size(fill, fill_w, h - 2);
        lv_obj_set_style_bg_color(fill, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(fill, 0, 0);
        lv_obj_set_style_pad_all(fill, 0, 0);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    }
}

/**
 * @brief 取限额条目的友好显示名
 *
 * TIME_LIMIT 映射为"MCP 月额度"；TOKENS_LIMIT 按 short_tokens_idx 区分短周期
 * （每5小时额度）与长周期（每周额度）；其余类型回退为原始 type 或占位"--"。
 *
 * @param v                 限额视图
 * @param idx               当前限额条目索引
 * @param short_tokens_idx  TOKENS_LIMIT 中 next_reset 最早的索引，用于区分短/长周期
 * @return 友好名静态字符串指针，始终非 NULL（指向字面量或视图内 type 字段）
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

/**
 * @brief 按限额视图状态绘制配额页主体内容
 *
 * @param body 页面内容区容器（调用前应为干净状态）
 * @param v    限额视图数据
 */
static void ui_quota_page_draw(lv_obj_t *body, const quota_view_model_t *v)
{
    const bool ok = (v->status == PRESENTATION_DATA_OK || v->status == PRESENTATION_DATA_STALE);
    char       buf[80];

    if (ok && v->limit_count > 0)
    {
        /* 找 TOKENS_LIMIT 里 next_reset 最早的（短周期=每5小时额度） */
        uint8_t short_tokens_idx = 0xFF;
        for (uint8_t i = 0; i < v->limit_count; ++i)
        {
            if (strcmp(v->limits[i].type, "TOKENS_LIMIT") != 0)
            {
                continue;
            }
            if (short_tokens_idx == 0xFF || strcmp(v->limits[i].next_reset, v->limits[short_tokens_idx].next_reset) < 0)
            {
                short_tokens_idx = i;
            }
        }

        const int32_t row_h = 70;
        const int32_t y0    = 10;
        const int32_t bar_x = 10;
        const int32_t bar_w = 300;
        const int32_t bar_h = 10;

        for (uint8_t i = 0; i < v->limit_count && i < QUOTA_VIEW_LIMIT_MAX; ++i)
        {
            const int32_t                  y = y0 + (int32_t) i * row_h;
            const quota_item_view_model_t *q = &v->limits[i];

            /* 第一行：友好名（左,text24）+ 重置时间（右,text16） */
            lv_obj_t *name                   = ui_common_new_text24(body);
            ui_common_set_label(name,
                                quota_friendly_name(v, i, short_tokens_idx),
                                bar_x,
                                y,
                                220,
                                24,
                                LV_TEXT_ALIGN_LEFT);

            if (q->next_reset[0])
            {
                /* next_reset "YYYY-MM-DD HH:MM" → 截 MM-DD HH:MM（跳过年份，取 index 5 起 11 字符） */
                snprintf(buf, sizeof(buf), "重置 %.11s", q->next_reset + 5);
                lv_obj_t *reset = ui_common_new_text16(body);
                ui_common_set_label(reset, buf, 230, y + 6, 160, 18, LV_TEXT_ALIGN_RIGHT);
            }

            /* 第二行：进度条 + 百分比（>80% 反白预警） */
            draw_progress_bar(body, bar_x, y + 32, bar_w, bar_h, q->used_percent);

            snprintf(buf, sizeof(buf), "%.0f%%", (double) q->used_percent);
            lv_obj_t *pct = (q->used_percent > 80.0f) ? ui_common_new_inverse_text16(body) : ui_common_new_text16(body);
            ui_common_set_label(pct, buf, bar_x + bar_w + 6, y + 28, 60, 18, LV_TEXT_ALIGN_RIGHT);
        }
    }
    else if (ok)
    {
        /* OK 但 limits 为空：服务端返回了空列表 */
        lv_obj_t *empty = ui_common_new_text24(body);
        ui_common_set_label(empty, "暂无限额数据", 10, 110, 380, 26, LV_TEXT_ALIGN_CENTER);
    }
    else if (v->status == PRESENTATION_DATA_ERROR)
    {
        /* 查询失败：主体居中反白提示 + 小字 error */
        lv_obj_t *err = ui_common_new_inverse_text16(body);
        ui_common_set_label(err, "限额查询失败", 10, 100, 380, 20, LV_TEXT_ALIGN_CENTER);
        if (v->error[0])
        {
            lv_obj_t *detail = ui_common_new_text16(body);
            ui_common_set_label(detail, v->error, 10, 124, 380, 18, LV_TEXT_ALIGN_CENTER);
        }
    }
    else
    {
        /* EMPTY / 首次未拉到 */
        lv_obj_t *loading = ui_common_new_text16(body);
        ui_common_set_label(loading, "正在加载限额…", 10, 120, 380, 18, LV_TEXT_ALIGN_CENTER);
    }

    /* 底部：更新时间（数据时效；每项已有各自重置时间，不再汇总） */
    if (ok && v->updated_at[0])
    {
        ui_common_new_hline(body, 230);
        snprintf(buf, sizeof(buf), "更新 %.16s", v->updated_at);
        char *t = strchr(buf, 'T');
        if (t != NULL)
        {
            *t = ' ';
        }
        lv_obj_t *upd = ui_common_new_text16(body);
        ui_common_set_label(upd, buf, 240, 238, 150, 18, LV_TEXT_ALIGN_RIGHT);
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

    quota_view_model_t view;
    quota_presenter_get_view_copy(&view);

    lv_obj_clean(parent);
    ui_quota_page_draw(parent, &view);
    return ESP_OK;
}

/* 数据刷新：复用 _show 的 clean + draw 路径做局部重绘，不走 switch_to 以避免重复应用功耗/刷新策略。 */
esp_err_t ui_quota_page_update(lv_obj_t *parent)
{
    return ui_quota_page_show(parent);
}
