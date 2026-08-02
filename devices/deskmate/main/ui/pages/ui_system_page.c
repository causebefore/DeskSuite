/*
 * 文件职责：实现设置菜单内的系统信息子页面。
 * 主要依赖：ui_common、ui_format、system_presenter。
 * 调用方：ui_settings_page。
 */
#include "ui_system_page.h"

#include "system_presenter.h"
#include "ui_common.h"
#include "ui_format.h"

#include <stdio.h>

esp_err_t ui_system_page_init(void)
{
    return ESP_OK;
}

/**
 * @brief 绘制系统诊断信息
 *
 * @param body 页面内容区容器（调用前应为干净状态）
 * @param s    系统视图（版本 / 构建 / 运行时间 / SRAM / PSRAM / CPU）
 */
static void ui_system_page_draw(lv_obj_t *body, const system_info_view_model_t *s)
{
    const bool sys_ok = (s->status == PRESENTATION_DATA_OK);
    char       buf[80];
    char       tmp[32];
    int32_t    y = 8;

    lv_obj_t *l  = ui_common_new_text16_regular(body);
    snprintf(buf, sizeof(buf), "版本  %s", sys_ok ? s->version : "--");
    ui_common_set_label(l, buf, 18, y, 360, 20, LV_TEXT_ALIGN_LEFT);
    y += 30;

    l = ui_common_new_text16_regular(body);
    snprintf(buf, sizeof(buf), "构建  %s", sys_ok ? s->build_time : "--");
    ui_common_set_label(l, buf, 18, y, 360, 20, LV_TEXT_ALIGN_LEFT);
    y += 30;

    l = ui_common_new_text16_regular(body);
    if (sys_ok)
    {
        ui_format_uptime(s->uptime_sec, tmp, sizeof(tmp));
        snprintf(buf, sizeof(buf), "运行  %s", tmp);
    }
    else
    {
        snprintf(buf, sizeof(buf), "运行  --");
    }
    ui_common_set_label(l, buf, 18, y, 360, 20, LV_TEXT_ALIGN_LEFT);
    y += 34;

    l = ui_common_new_text16_regular(body);
    if (sys_ok)
    {
        ui_format_memory(s->sram_free_kb, s->sram_total_kb, s->sram_used_percent, tmp, sizeof(tmp));
        snprintf(buf, sizeof(buf), "SRAM  %s", tmp);
    }
    else
    {
        snprintf(buf, sizeof(buf), "SRAM  --");
    }
    ui_common_set_label(l, buf, 18, y, 360, 20, LV_TEXT_ALIGN_LEFT);
    y += 30;

    l = ui_common_new_text16_regular(body);
    if (sys_ok)
    {
        ui_format_memory(s->psram_free_kb, s->psram_total_kb, s->psram_used_percent, tmp, sizeof(tmp));
        snprintf(buf, sizeof(buf), "PSRAM %s", tmp);
    }
    else
    {
        snprintf(buf, sizeof(buf), "PSRAM --");
    }
    ui_common_set_label(l, buf, 18, y, 360, 20, LV_TEXT_ALIGN_LEFT);
    y += 30;

    l = ui_common_new_text16_regular(body);
    if (sys_ok)
    {
        snprintf(buf, sizeof(buf), "CPU   %uMHz", (unsigned) s->cpu_mhz);
    }
    else
    {
        snprintf(buf, sizeof(buf), "CPU   --");
    }
    ui_common_set_label(l, buf, 18, y, 360, 20, LV_TEXT_ALIGN_LEFT);
}

esp_err_t ui_system_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    system_page_view_model_t view;
    system_presenter_get_view_copy(&view);

    lv_obj_clean(parent);
    ui_system_page_draw(parent, &view.system);
    return ESP_OK;
}

/* 数据刷新：复用 _show 的 clean + draw 路径做局部重绘。 */
esp_err_t ui_system_page_update(lv_obj_t *parent)
{
    return ui_system_page_show(parent);
}
