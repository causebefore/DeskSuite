/*
 * 文件职责：解析 RLCD 字体容器头和索引，提供与渲染框架无关的块访问接口。
 * 主要依赖：font_config.h。
 * 调用方：ui_platform 字体运行时和其他字体装载器。
 */
#ifndef RLCD_FONT_CONTAINER_H
#define RLCD_FONT_CONTAINER_H

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    RLCD_FONT_CONTAINER_OK = 0,
    RLCD_FONT_CONTAINER_INVALID_ARG,
    RLCD_FONT_CONTAINER_INVALID_MAGIC,
    RLCD_FONT_CONTAINER_INVALID_HEADER,
    RLCD_FONT_CONTAINER_INVALID_INDEX,
    RLCD_FONT_CONTAINER_INDEX_OUT_OF_RANGE,
    RLCD_FONT_CONTAINER_BLOCK_OUT_OF_RANGE,
} rlcd_font_container_status_t;

typedef struct
{
    const uint8_t *base;
    size_t         size;
    uint16_t       count;
} rlcd_font_container_t;

typedef struct
{
    uint8_t        id;
    uint16_t       size_px;
    uint32_t       offset;
    uint32_t       length;
    const uint8_t *data;
} rlcd_font_block_t;

rlcd_font_container_status_t rlcd_font_container_open(const uint8_t *data, size_t len,
                                                      rlcd_font_container_t *out_container);

rlcd_font_container_status_t rlcd_font_container_get_block(const rlcd_font_container_t *container, uint16_t index,
                                                           rlcd_font_block_t *out_block);

#endif /* RLCD_FONT_CONTAINER_H */
