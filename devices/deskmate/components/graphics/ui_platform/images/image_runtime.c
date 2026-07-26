#include "ui_platform_image.h"

#include "qweather_i1_assets.h"
#include "status_i1_assets.h"

static const ui_platform_image_catalog_t s_qweather_catalog = {
    .name        = "qweather",
    .entries     = qweather_i1_entries,
    .entry_count = QWEATHER_I1_ENTRY_COUNT,
};

static const ui_platform_image_catalog_t s_status_catalog = {
    .name        = "status",
    .entries     = status_i1_entries,
    .entry_count = STATUS_I1_ENTRY_COUNT,
};

const ui_platform_image_catalog_t *ui_platform_image_qweather_catalog(void)
{
    return &s_qweather_catalog;
}

const ui_platform_image_catalog_t *ui_platform_image_status_catalog(void)
{
    return &s_status_catalog;
}

static int compare(uint32_t key, ui_platform_image_variant_t variant, const ui_platform_image_entry_t *entry)
{
    if (key != entry->key)
    {
        return key < entry->key ? -1 : 1;
    }
    if (variant == entry->variant)
    {
        return 0;
    }
    return variant < entry->variant ? -1 : 1;
}

const lv_image_dsc_t *ui_platform_image_find(const ui_platform_image_catalog_t *catalog, uint32_t key,
                                             ui_platform_image_variant_t variant)
{
    if (catalog == NULL || catalog->entries == NULL)
    {
        return NULL;
    }
    size_t left  = 0;
    size_t right = catalog->entry_count;
    while (left < right)
    {
        const size_t middle = left + (right - left) / 2;
        const int    order  = compare(key, variant, &catalog->entries[middle]);
        if (order == 0)
        {
            return catalog->entries[middle].image;
        }
        if (order < 0)
        {
            right = middle;
        }
        else
        {
            left = middle + 1;
        }
    }
    return NULL;
}

esp_err_t ui_platform_image_apply(lv_obj_t *image_obj, const ui_platform_image_catalog_t *catalog, uint32_t key,
                                  ui_platform_image_variant_t variant, bool hide_when_missing)
{
    if (image_obj == NULL || catalog == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const lv_image_dsc_t *image = ui_platform_image_find(catalog, key, variant);
    if (image == NULL)
    {
        if (hide_when_missing)
        {
            lv_obj_add_flag(image_obj, LV_OBJ_FLAG_HIDDEN);
        }
        return ESP_ERR_NOT_FOUND;
    }
    lv_image_set_src(image_obj, image);
    lv_obj_remove_flag(image_obj, LV_OBJ_FLAG_HIDDEN);
    return ESP_OK;
}
