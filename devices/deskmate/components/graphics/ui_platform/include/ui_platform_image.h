/*
 * 文件职责：定义静态 I1 图片资源目录和通用查询接口。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

typedef enum
{
    UI_PLATFORM_IMAGE_VARIANT_DEFAULT = 0,
    UI_PLATFORM_IMAGE_VARIANT_SMALL,
    UI_PLATFORM_IMAGE_VARIANT_LARGE,
} ui_platform_image_variant_t;

typedef struct
{
    uint32_t                    key;
    ui_platform_image_variant_t variant;
    const lv_image_dsc_t       *image;
} ui_platform_image_entry_t;

typedef struct ui_platform_image_catalog
{
    const char                      *name;
    const ui_platform_image_entry_t *entries;
    size_t                           entry_count;
} ui_platform_image_catalog_t;

const ui_platform_image_catalog_t *ui_platform_image_qweather_catalog(void);
const ui_platform_image_catalog_t *ui_platform_image_status_catalog(void);
const lv_image_dsc_t              *ui_platform_image_find(const ui_platform_image_catalog_t *catalog, uint32_t key,
                                                          ui_platform_image_variant_t variant);
