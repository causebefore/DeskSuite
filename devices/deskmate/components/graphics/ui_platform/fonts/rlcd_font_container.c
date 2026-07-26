/*
 * 文件职责：解析 RLCD 字体容器头和索引。
 * 设计目标：不依赖 ESP-IDF、LVGL，仅做边界校验和块定位。
 */
#include "rlcd_font_container.h"

#include "font_config.h"

#include <stdbool.h>
#include <string.h>

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static bool range_ok(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

rlcd_font_container_status_t rlcd_font_container_open(const uint8_t *data, size_t len,
                                                      rlcd_font_container_t *out_container)
{
    if (data == NULL || out_container == NULL)
    {
        return RLCD_FONT_CONTAINER_INVALID_ARG;
    }

    if (!range_ok(0, RLCD_FONT_HEADER_SIZE, len) || memcmp(data, RLCD_FONT_MAGIC, 4) != 0)
    {
        return RLCD_FONT_CONTAINER_INVALID_MAGIC;
    }

    uint16_t version = rd_u16(data + 4);
    uint16_t count   = rd_u16(data + 6);
    if (version != RLCD_FONT_VERSION || count == 0)
    {
        return RLCD_FONT_CONTAINER_INVALID_HEADER;
    }

    size_t index_bytes = (size_t) count * RLCD_FONT_INDEX_ENTRY_SIZE;
    if (count > RLCD_FONT_MAX_BLOCKS || !range_ok(RLCD_FONT_HEADER_SIZE, index_bytes, len))
    {
        return RLCD_FONT_CONTAINER_INVALID_INDEX;
    }

    out_container->base  = data;
    out_container->size  = len;
    out_container->count = count;
    return RLCD_FONT_CONTAINER_OK;
}

rlcd_font_container_status_t rlcd_font_container_get_block(const rlcd_font_container_t *container, uint16_t index,
                                                           rlcd_font_block_t *out_block)
{
    if (container == NULL || out_block == NULL || container->base == NULL)
    {
        return RLCD_FONT_CONTAINER_INVALID_ARG;
    }

    if (index >= container->count)
    {
        return RLCD_FONT_CONTAINER_INDEX_OUT_OF_RANGE;
    }

    const uint8_t *entry  = container->base + RLCD_FONT_HEADER_SIZE + (size_t) index * RLCD_FONT_INDEX_ENTRY_SIZE;
    uint32_t       offset = rd_u32(entry + 3);
    uint32_t       length = rd_u32(entry + 7);
    if (!range_ok(offset, length, container->size))
    {
        return RLCD_FONT_CONTAINER_BLOCK_OUT_OF_RANGE;
    }

    out_block->id      = entry[0];
    out_block->size_px = rd_u16(entry + 1);
    out_block->offset  = offset;
    out_block->length  = length;
    out_block->data    = container->base + offset;
    return RLCD_FONT_CONTAINER_OK;
}
