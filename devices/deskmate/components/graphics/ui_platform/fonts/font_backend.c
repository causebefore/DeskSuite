/*
 * 文件职责：组合 RLCD 容器层和 LVGL 适配层，完成字体库整体装载。
 * 设计目标：集中完成“遍历块、按 ID 分发、校验必需字体”。
 */
#include "font_backend.h"

#include "bin_font_lvgl_adapter.h"
#include "font_config.h"
#include "rlcd_font_container.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "font_backend";

static lv_font_t *slot_by_id(font_backend_runtime_t *runtime, uint8_t id)
{
    switch (id)
    {
        case RLCD_FONT_ID_ZH_16:
            return &runtime->font_zh_16;
        case RLCD_FONT_ID_ZH_24:
            return &runtime->font_zh_24;
        case RLCD_FONT_ID_ZH_32:
            return &runtime->font_zh_32;
        case RLCD_FONT_ID_ZH_16_SEMIBOLD:
            return &runtime->font_zh_16_semibold;
        case RLCD_FONT_ID_ZH_24_SEMIBOLD:
            return &runtime->font_zh_24_semibold;
        case RLCD_FONT_ID_NUM_48:
            return &runtime->font_num_48;
        default:
            return NULL;
    }
}

static esp_err_t map_container_status(rlcd_font_container_status_t status)
{
    switch (status)
    {
        case RLCD_FONT_CONTAINER_OK:
            return ESP_OK;
        case RLCD_FONT_CONTAINER_INVALID_MAGIC:
            return ESP_ERR_INVALID_STATE;
        case RLCD_FONT_CONTAINER_INVALID_HEADER:
            return ESP_ERR_INVALID_VERSION;
        case RLCD_FONT_CONTAINER_INVALID_INDEX:
        case RLCD_FONT_CONTAINER_BLOCK_OUT_OF_RANGE:
        case RLCD_FONT_CONTAINER_INDEX_OUT_OF_RANGE:
            return ESP_ERR_INVALID_SIZE;
        case RLCD_FONT_CONTAINER_INVALID_ARG:
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static void log_container_open_error(rlcd_font_container_status_t status)
{
    switch (status)
    {
        case RLCD_FONT_CONTAINER_INVALID_MAGIC:
            ESP_LOGE(TAG, "font.bin magic mismatch");
            break;
        case RLCD_FONT_CONTAINER_INVALID_HEADER:
            ESP_LOGE(TAG, "font.bin header invalid");
            break;
        case RLCD_FONT_CONTAINER_INVALID_INDEX:
            ESP_LOGE(TAG, "font.bin index invalid");
            break;
        case RLCD_FONT_CONTAINER_INVALID_ARG:
        default:
            ESP_LOGE(TAG, "font container open failed: status=%d", (int) status);
            break;
    }
}

static void log_container_block_error(uint16_t index, rlcd_font_container_status_t status)
{
    switch (status)
    {
        case RLCD_FONT_CONTAINER_BLOCK_OUT_OF_RANGE:
            ESP_LOGE(TAG, "font block out of range: index=%u", index);
            break;
        case RLCD_FONT_CONTAINER_INDEX_OUT_OF_RANGE:
            ESP_LOGE(TAG, "font block index invalid: index=%u", index);
            break;
        case RLCD_FONT_CONTAINER_INVALID_ARG:
        default:
            ESP_LOGE(TAG, "font block read failed: index=%u status=%d", index, (int) status);
            break;
    }
}

void font_backend_unload(font_backend_runtime_t *runtime)
{
    if (runtime == NULL)
    {
        return;
    }

    bin_font_lvgl_adapter_free(&runtime->font_zh_16);
    bin_font_lvgl_adapter_free(&runtime->font_zh_24);
    bin_font_lvgl_adapter_free(&runtime->font_zh_32);
    bin_font_lvgl_adapter_free(&runtime->font_zh_16_semibold);
    bin_font_lvgl_adapter_free(&runtime->font_zh_24_semibold);
    bin_font_lvgl_adapter_free(&runtime->font_num_48);
    memset(runtime, 0, sizeof(*runtime));
}

esp_err_t font_backend_load(const uint8_t *data, size_t len, font_backend_runtime_t *runtime, uint16_t *out_block_count)
{
    if (data == NULL || runtime == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    font_backend_unload(runtime);

    rlcd_font_container_t        container;
    rlcd_font_container_status_t container_status = rlcd_font_container_open(data, len, &container);
    if (container_status != RLCD_FONT_CONTAINER_OK)
    {
        log_container_open_error(container_status);
        return map_container_status(container_status);
    }

    uint32_t loaded_mask = 0;
    for (uint16_t i = 0; i < container.count; i++)
    {
        rlcd_font_block_t block;
        container_status = rlcd_font_container_get_block(&container, i, &block);
        if (container_status != RLCD_FONT_CONTAINER_OK)
        {
            log_container_block_error(i, container_status);
            font_backend_unload(runtime);
            return map_container_status(container_status);
        }

        lv_font_t *slot = slot_by_id(runtime, block.id);
        if (slot == NULL)
        {
            ESP_LOGW(TAG, "ignore unknown font id=%u size=%upx", block.id, block.size_px);
            continue;
        }

        if (!bin_font_lvgl_adapter_build(block.data, block.length, slot))
        {
            ESP_LOGE(TAG, "parse font failed: id=%u size=%upx", block.id, block.size_px);
            font_backend_unload(runtime);
            return ESP_FAIL;
        }

        loaded_mask |= (1U << block.id);
        ESP_LOGI(TAG, "font loaded: id=%u size=%upx", block.id, block.size_px);
    }

    if ((loaded_mask & RLCD_FONT_REQUIRED_MASK) != RLCD_FONT_REQUIRED_MASK)
    {
        ESP_LOGE(TAG, "font.bin missing required blocks: mask=0x%lx", (unsigned long) loaded_mask);
        font_backend_unload(runtime);
        return ESP_ERR_NOT_FOUND;
    }

    if (out_block_count != NULL)
    {
        *out_block_count = container.count;
    }

    return ESP_OK;
}
