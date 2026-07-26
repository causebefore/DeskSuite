/*
 * 文件职责：从 font 分区加载字体并以可恢复回退方式提供给 UI。
 */
#include "ui_platform_font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "font_backend.h"
#include "spi_flash_mmap.h"
#include "system_partition.h"

static const char *TAG = "ui_platform_font";

static spi_flash_mmap_handle_t   s_mmap_handle;
static const uint8_t            *s_font_base;
static size_t                    s_font_size;
static font_backend_runtime_t    s_runtime;
static ui_platform_font_status_t s_status = UI_PLATFORM_FONT_UNAVAILABLE;
static bool                      s_initialized;

static void release_mapping(void)
{
    if (s_font_base != NULL)
    {
        spi_flash_munmap(s_mmap_handle);
        s_font_base = NULL;
        s_font_size = 0;
    }
}

static void use_fallback(const char *reason, esp_err_t err)
{
    font_backend_unload(&s_runtime);
    release_mapping();
    s_status      = UI_PLATFORM_FONT_FALLBACK;
    s_initialized = true;
    ESP_LOGW(TAG, "字体运行时降级到 LV_FONT_DEFAULT: reason=%s err=%s", reason, esp_err_to_name(err));
}

esp_err_t ui_platform_font_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           (esp_partition_subtype_t) SYSTEM_PARTITION_FONT_SUBTYPE,
                                                           SYSTEM_PARTITION_FONT_LABEL);
    if (part == NULL)
    {
        use_fallback("font_partition_not_found", ESP_ERR_NOT_FOUND);
        return ESP_OK;
    }

    esp_err_t err =
        esp_partition_mmap(part, 0, part->size, SPI_FLASH_MMAP_DATA, (const void **) &s_font_base, &s_mmap_handle);
    if (err != ESP_OK || s_font_base == NULL)
    {
        use_fallback("font_partition_mmap_failed", err == ESP_OK ? ESP_FAIL : err);
        return ESP_OK;
    }
    s_font_size          = part->size;

    uint16_t block_count = 0;
    err                  = font_backend_load(s_font_base, s_font_size, &s_runtime, &block_count);
    if (err != ESP_OK)
    {
        use_fallback("font_container_invalid", err);
        return ESP_OK;
    }

    s_status      = UI_PLATFORM_FONT_READY;
    s_initialized = true;
    ESP_LOGI(TAG, "字体运行时初始化完成: blocks=%u size=0x%lx", block_count, (unsigned long) part->size);
    return ESP_OK;
}

void ui_platform_font_deinit(void)
{
    font_backend_unload(&s_runtime);
    release_mapping();
    s_status      = UI_PLATFORM_FONT_UNAVAILABLE;
    s_initialized = false;
}

ui_platform_font_status_t ui_platform_font_get_status(void)
{
    return s_status;
}

const lv_font_t *ui_platform_font_get(uint16_t size_px)
{
    if (s_status != UI_PLATFORM_FONT_READY)
    {
        return LV_FONT_DEFAULT;
    }
    switch (size_px)
    {
        case 16:
            return &s_runtime.font_zh_16;
        case 24:
            return &s_runtime.font_zh_24;
        case 32:
            return &s_runtime.font_zh_32;
        case 48:
            return &s_runtime.font_num_48;
        default:
            return LV_FONT_DEFAULT;
    }
}

const lv_font_t *ui_platform_font_get_semibold(uint16_t size_px)
{
    if (s_status != UI_PLATFORM_FONT_READY)
    {
        return LV_FONT_DEFAULT;
    }
    switch (size_px)
    {
        case 16:
            return &s_runtime.font_zh_16_semibold;
        case 24:
            return &s_runtime.font_zh_24_semibold;
        default:
            return ui_platform_font_get(size_px);
    }
}

static uint32_t utf8_next(const char *text, uint32_t *index)
{
    const uint8_t *bytes = (const uint8_t *) text;
    const uint8_t  first = bytes[*index];
    if (first < 0x80)
    {
        (*index)++;
        return first;
    }
    if ((first & 0xE0) == 0xC0 && bytes[*index + 1] != 0)
    {
        const uint32_t value = ((uint32_t) (first & 0x1F) << 6) | (uint32_t) (bytes[*index + 1] & 0x3F);
        *index += 2;
        return value;
    }
    if ((first & 0xF0) == 0xE0 && bytes[*index + 1] != 0 && bytes[*index + 2] != 0)
    {
        const uint32_t value = ((uint32_t) (first & 0x0F) << 12) | ((uint32_t) (bytes[*index + 1] & 0x3F) << 6)
                               | (uint32_t) (bytes[*index + 2] & 0x3F);
        *index += 3;
        return value;
    }
    if ((first & 0xF8) == 0xF0 && bytes[*index + 1] != 0 && bytes[*index + 2] != 0 && bytes[*index + 3] != 0)
    {
        const uint32_t value = ((uint32_t) (first & 0x07) << 18) | ((uint32_t) (bytes[*index + 1] & 0x3F) << 12)
                               | ((uint32_t) (bytes[*index + 2] & 0x3F) << 6) | (uint32_t) (bytes[*index + 3] & 0x3F);
        *index += 4;
        return value;
    }
    (*index)++;
    return 0xFFFD;
}

uint16_t ui_platform_font_measure_text(const lv_font_t *font, const char *text)
{
    if (font == NULL || text == NULL)
    {
        return 0;
    }

    uint32_t width = 0;
    uint32_t index = 0;
    while (text[index] != '\0')
    {
        uint32_t       next_index     = index;
        const uint32_t codepoint      = utf8_next(text, &next_index);
        uint32_t       lookahead      = next_index;
        const uint32_t next_codepoint = text[lookahead] == '\0' ? 0 : utf8_next(text, &lookahead);
        width += (uint32_t) lv_font_get_glyph_width(font, codepoint, next_codepoint);
        index = next_index;
    }
    return width > UINT16_MAX ? UINT16_MAX : (uint16_t) width;
}
