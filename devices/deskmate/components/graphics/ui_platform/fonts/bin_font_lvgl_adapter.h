/*
 * 文件职责：把通用 bin 字体描述适配为 LVGL 可消费的 lv_font_t。
 * 主要依赖：LVGL、bin_font_probe。
 * 调用方：ui_platform 字体运行时和其他 LVGL 渲染入口。
 */
#ifndef BIN_FONT_LVGL_ADAPTER_H
#define BIN_FONT_LVGL_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _lv_font_t lv_font_t;

/**
 * 把单个 LVGL binfont 块构造成可用的 lv_font_t。
 *
 * 输入数据必须在 out_font 生命周期内保持常驻（典型场景：mmap 到 SPI flash）。
 * 部分 LVGL 9 运行时结构由 lv_malloc 分配在堆上；cmap 子表指针仍指向 data。
 */
bool bin_font_lvgl_adapter_build(const uint8_t *data, size_t len, lv_font_t *out_font);

/** 释放由 bin_font_lvgl_adapter_build 构造的 LVGL 字体运行时结构。 */
void bin_font_lvgl_adapter_free(lv_font_t *font);

#endif /* BIN_FONT_LVGL_ADAPTER_H */
