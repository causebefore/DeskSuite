/*
 * 文件职责：组合容器层和渲染适配层，完成整套字体的加载与释放。
 * 主要依赖：RLCD 容器解析、LVGL 字体适配。
 * 调用方：ui_platform 字体运行时。
 */
#ifndef FONT_BACKEND_H
#define FONT_BACKEND_H

#include "esp_err.h"
#include "lvgl.h"

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    lv_font_t font_zh_16;
    lv_font_t font_zh_24;
    lv_font_t font_zh_32;
    lv_font_t font_zh_16_semibold;
    lv_font_t font_zh_24_semibold;
    lv_font_t font_num_48;
} font_backend_runtime_t;

esp_err_t font_backend_load(const uint8_t *data, size_t len, font_backend_runtime_t *runtime,
                            uint16_t *out_block_count);

void font_backend_unload(font_backend_runtime_t *runtime);

#endif /* FONT_BACKEND_H */
