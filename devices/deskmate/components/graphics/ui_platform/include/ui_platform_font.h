/*
 * 文件职责：提供字体分区、字体运行时和 LVGL 字体查询接口。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

typedef enum
{
    UI_PLATFORM_FONT_READY = 0,
    UI_PLATFORM_FONT_FALLBACK,
    UI_PLATFORM_FONT_UNAVAILABLE,
} ui_platform_font_status_t;

esp_err_t                 ui_platform_font_init(void);
void                      ui_platform_font_deinit(void);
ui_platform_font_status_t ui_platform_font_get_status(void);
const lv_font_t          *ui_platform_font_get(uint16_t size_px);
const lv_font_t          *ui_platform_font_get_semibold(uint16_t size_px);
uint16_t                  ui_platform_font_measure_text(const lv_font_t *font, const char *text);
