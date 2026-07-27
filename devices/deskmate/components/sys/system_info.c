/**
 * @file system_info.c
 * @brief 无 Task、无生命周期状态的系统只读信息采集
 */
#include "system_info.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"

static uint8_t calculate_used_percent(uint32_t total, uint32_t free)
{
    if (total == 0)
    {
        return 0;
    }
    const uint32_t used = total - free;
    return (uint8_t) ((used * 100U + total / 2U) / total);
}

esp_err_t system_info_get_snapshot_copy(system_info_snapshot_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const esp_app_desc_t *description = esp_app_get_description();
    if (description != NULL)
    {
        snprintf(out->version, sizeof(out->version), "%s", description->version);
        snprintf(out->build_time, sizeof(out->build_time), "%.8s %.8s", description->date, description->time);
    }
    out->uptime_sec           = (uint32_t) (esp_timer_get_time() / 1000000ULL);

    const uint32_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const uint32_t sram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->sram_total_kb        = sram_total / 1024U;
    out->sram_free_kb         = sram_free / 1024U;
    out->sram_used_percent    = calculate_used_percent(sram_total, sram_free);

    if (esp_psram_get_size() > 0)
    {
        const uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const uint32_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        out->psram_total_kb        = psram_total / 1024U;
        out->psram_free_kb         = psram_free / 1024U;
        out->psram_used_percent    = calculate_used_percent(psram_total, psram_free);
    }
    out->cpu_mhz = (uint16_t) (esp_clk_cpu_freq() / 1000000U);
    out->valid   = true;
    return ESP_OK;
}

const char *system_info_get_firmware_version_borrow(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    return description != NULL ? description->version : "";
}

const char *system_info_get_reset_reason_borrow(void)
{
    switch (esp_reset_reason())
    {
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return "watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        default:
            return "unknown";
    }
}
