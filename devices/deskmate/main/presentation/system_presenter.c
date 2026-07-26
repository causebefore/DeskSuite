/*
 * 文件职责：把系统只读事实转换为系统信息页 View Model。
 */
#include "system_presenter.h"

#include <string.h>

#include "system_info.h"

static system_page_view_model_t s_view;

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0)
    {
        return;
    }
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    const size_t copy_len = strnlen(src, dst_len - 1U);
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

esp_err_t system_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    s_view.system.status = PRESENTATION_DATA_EMPTY;
    return ESP_OK;
}

void system_presenter_get_view_copy(system_page_view_model_t *out_view)
{
    system_info_snapshot_t sys;
    if (system_info_get_snapshot_copy(&sys) != ESP_OK)
    {
        return;
    }
    if (sys.valid)
    {
        copy_text(s_view.system.version, sizeof(s_view.system.version), sys.version);
        copy_text(s_view.system.build_time, sizeof(s_view.system.build_time), sys.build_time);
        s_view.system.uptime_sec         = sys.uptime_sec;
        s_view.system.sram_total_kb      = sys.sram_total_kb;
        s_view.system.sram_free_kb       = sys.sram_free_kb;
        s_view.system.sram_used_percent  = sys.sram_used_percent;
        s_view.system.psram_total_kb     = sys.psram_total_kb;
        s_view.system.psram_free_kb      = sys.psram_free_kb;
        s_view.system.psram_used_percent = sys.psram_used_percent;
        s_view.system.cpu_mhz            = sys.cpu_mhz;
        s_view.system.status             = PRESENTATION_DATA_OK;
    }

    if (out_view != NULL)
    {
        *out_view = s_view;
    }
}
