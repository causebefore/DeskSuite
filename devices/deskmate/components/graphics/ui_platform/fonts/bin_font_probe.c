/**
 * @file bin_font_probe.c
 *
 * 纯 C、零 LVGL 依赖的 LVGL bin 字体格式探针实现。
 *
 * 移植自 LVGL 9 的 src/font/lv_binfont_loader.c（MIT 许可，版权归 LVGL authors）。
 * 改造点：
 *   1) 数据源：原版用 lv_fs_read 顺序读文件 → 本实现直接按偏移访问 const uint8_t *data；
 *   2) 内存：  原版对每段 lv_malloc+memcpy 拷贝 → 本实现零拷贝，所有指针指向 data 区间；
 *   3) glyph 解码：原版在 load_glyph 里按位宽解码出 glyph_dsc[] 与连续 glyph_bitmap。
 *      本实现只记录 glyf 块体起点/长度与 loca 表，把解码工作留给 device 侧
 *      bin_font_parse.c（host 测试不需要解码 glyph 即可验证格式正确性）。
 */
#include "bin_font_probe.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* 小端读取（bin 字体固定为小端）                                       */
/* ------------------------------------------------------------------ */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int16_t rd_i16(const uint8_t *p)
{
    return (int16_t) rd_u16(p);
}

/* ------------------------------------------------------------------ */
/* 边界检查助手                                                        */
/* ------------------------------------------------------------------ */

/** 检查 [off, off+need) 是否完全落在 [0, len) 内。 */
static int range_ok(size_t off, size_t need, size_t len)
{
    return off <= len && need <= (len - off);
}

static uint32_t read_loca_offset(const uint8_t *table, uint32_t index, uint8_t format)
{
    if (format == 1)
    {
        return rd_u32(table + (size_t) index * sizeof(uint32_t));
    }
    return rd_u16(table + (size_t) index * sizeof(uint16_t));
}

/**
 * 读取一个带标签的块头（等价于 v9 的 read_label，但不前进游标）。
 *
 * LVGL bin 块约定：[uint32_t length][4 字节标签][length-8 字节块体]。
 * length 是整块字节数（含 8 字节头），块体长 = length - 8。
 */
static int read_block(const uint8_t *data, size_t len, size_t off, const char *label, size_t *body_off,
                      uint32_t *body_len, size_t *next_off)
{
    if (!range_ok(off, 8, len))
    {
        return 0;
    }
    uint32_t total = rd_u32(data + off);
    if (total < 8)
    {
        return 0;
    }
    if (memcmp(data + off + 4, label, 4) != 0)
    {
        return 0;
    }
    if (!range_ok(off, total, len))
    {
        return 0;
    }
    *body_off = off + 8;
    *body_len = total - 8;
    if (next_off)
    {
        *next_off = off + total;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 主解析                                                              */
/* ------------------------------------------------------------------ */

bool bin_font_probe(const uint8_t *data, size_t len, bin_font_desc_t *out)
{
    if (data == NULL || out == NULL || len < 8)
    {
        return 0;
    }
    memset(out, 0, sizeof(*out));

    /* ---- head 块 ---- */
    size_t   head_body_off;
    uint32_t head_body_len;
    size_t   cmap_start;
    if (!read_block(data, len, 0, "head", &head_body_off, &head_body_len, &cmap_start))
    {
        return 0;
    }

    /* font_header_bin_t 字段布局（与 v9 lv_binfont_loader.c 完全一致）。
       紧凑二进制布局，逐字段按偏移读。共 40 字节。 */
    const size_t HDR = 40;
    if (head_body_len < HDR)
    {
        return 0;
    }
    const uint8_t *h           = data + head_body_off;
    out->tables_count          = rd_u16(h + 4);
    out->font_size             = rd_u16(h + 6);
    out->ascent                = rd_u16(h + 8);
    out->descent               = rd_i16(h + 10);
    out->default_advance_width = rd_u16(h + 22);
    out->kerning_scale         = rd_u16(h + 24);
    out->index_to_loc_format   = h[26];
    out->glyph_id_format       = h[27];
    out->advance_width_format  = h[28];
    out->bits_per_pixel        = h[29];
    out->xy_bits               = h[30];
    out->wh_bits               = h[31];
    out->advance_width_bits    = h[32];
    out->bitmap_format         = h[33];
    out->subpixels_mode        = h[34];
    out->underline_position    = rd_i16(h + 36);
    out->underline_thickness   = rd_u16(h + 38);

    /* 合理性校验 */
    if (out->index_to_loc_format > 1)
    {
        return 0;
    }
    if (out->bits_per_pixel == 0 || out->bits_per_pixel == 5 || out->bits_per_pixel == 6 || out->bits_per_pixel == 7
        || out->bits_per_pixel > 8)
    {
        return 0;
    }

    /* ---- cmap 块 ---- */
    size_t   cmap_body_off;
    uint32_t cmap_body_len;
    size_t   loca_start;
    if (!read_block(data, len, cmap_start, "cmap", &cmap_body_off, &cmap_body_len, &loca_start))
    {
        return 0;
    }

    if (cmap_body_len < 4)
    {
        return 0;
    }
    uint32_t cmap_num = rd_u32(data + cmap_body_off);
    if (cmap_num == 0 || cmap_num > BIN_FONT_MAX_CMAPS)
    {
        return 0;
    }
    const size_t CTAB     = 16;
    size_t       ctab_off = cmap_body_off + 4;
    if (!range_ok(ctab_off, (size_t) cmap_num * CTAB, len))
    {
        return 0;
    }

    out->cmap_num = (uint16_t) cmap_num;
    for (uint32_t i = 0; i < cmap_num; i++)
    {
        const uint8_t   *ct          = data + ctab_off + i * CTAB;
        bin_font_cmap_t *c           = &out->cmaps[i];
        uint32_t         data_offset = rd_u32(ct + 0);
        c->range_start               = rd_u32(ct + 4);
        c->range_length              = rd_u16(ct + 8);
        c->glyph_id_start            = rd_u16(ct + 10);
        c->list_length               = rd_u16(ct + 12);
        c->format_type               = ct[14];

        /* 各子表数据起点 = cmap 块起点 + data_offset */
        size_t sub_off               = cmap_start + data_offset;
        switch (c->format_type)
        {
            case 0: { /* FORMAT0_FULL */
                c->unicode_list = NULL;
                size_t need     = (size_t) c->range_length * sizeof(uint8_t);
                if (!range_ok(sub_off, need, len))
                {
                    return 0;
                }
                c->glyph_id_ofs_list = data + sub_off;
                c->list_length       = c->range_length;
                break;
            }
            case 2: { /* FORMAT0_TINY */
                c->unicode_list      = NULL;
                c->glyph_id_ofs_list = NULL;
                break;
            }
            case 1:   /* SPARSE_FULL */
            case 3: { /* SPARSE_TINY */
                size_t need = (size_t) c->list_length * sizeof(uint16_t);
                if (!range_ok(sub_off, need, len))
                {
                    return 0;
                }
                c->unicode_list = (const uint16_t *) (data + sub_off);
                size_t after    = sub_off + need;
                if (c->format_type == 1)
                {
                    size_t need2 = (size_t) c->list_length * sizeof(uint16_t);
                    if (!range_ok(after, need2, len))
                    {
                        return 0;
                    }
                    c->glyph_id_ofs_list = (const uint16_t *) (data + after);
                }
                else
                {
                    c->glyph_id_ofs_list = NULL;
                }
                break;
            }
            default:
                return 0;
        }
    }

    /* ---- loca 块 ---- */
    size_t   loca_body_off;
    uint32_t loca_body_len;
    size_t   glyf_start;
    if (!read_block(data, len, loca_start, "loca", &loca_body_off, &loca_body_len, &glyf_start))
    {
        return 0;
    }
    if (loca_body_len < 4)
    {
        return 0;
    }
    uint32_t loca_count = rd_u32(data + loca_body_off);
    if (loca_count == 0)
    {
        return 0;
    }
    size_t loca_tab_off = loca_body_off + 4;
    size_t elem         = (out->index_to_loc_format == 1) ? sizeof(uint32_t) : sizeof(uint16_t);
    if (!range_ok(loca_tab_off, (size_t) loca_count * elem, len))
    {
        return 0;
    }
    out->loca_table = data + loca_tab_off;
    out->loca_count = loca_count;

    /* ---- glyf 块 ---- */
    size_t   glyf_body_off;
    uint32_t glyf_body_len;
    size_t   kern_start;
    if (!read_block(data, len, glyf_start, "glyf", &glyf_body_off, &glyf_body_len, &kern_start))
    {
        return 0;
    }
    if (glyf_body_len == 0)
    {
        return 0;
    }
    (void) glyf_body_off;
    out->glyf_body     = data + glyf_start;
    out->glyf_body_len = (uint32_t) (kern_start - glyf_start);

    for (uint32_t i = 0; i < loca_count; i++)
    {
        uint32_t off = read_loca_offset(out->loca_table, i, out->index_to_loc_format);
        if (off > out->glyf_body_len)
        {
            return 0;
        }
        if (i + 1 < loca_count)
        {
            uint32_t next = read_loca_offset(out->loca_table, i + 1, out->index_to_loc_format);
            if (next < off)
            {
                return 0;
            }
        }
    }

    /* ---- kern 块（可选） ---- */
    if (out->tables_count >= 4)
    {
        size_t   kern_body_off;
        uint32_t kern_body_len;
        if (!read_block(data, len, kern_start, "kern", &kern_body_off, &kern_body_len, NULL))
        {
            return 0;
        }
        out->kern_body     = data + kern_body_off;
        out->kern_body_len = kern_body_len;
    }

    return 1;
}
