/**
 * @file bin_font_lvgl_adapter.c
 *
 * 把 bin_font_probe 解析出的通用字体描述适配成 LVGL 可用的 lv_font_t。
 *
 * 仅 device 编译：include lvgl.h，依赖 lv_malloc/lv_free 与
 * LVGL 9 的 lv_font_fmt_txt_dsc_t / lv_font_t 字段。
 *
 * 设计：
 *   - 调 bin_font_probe 得到零拷贝的 desc（各段指针指向入参 data，即 mmap 区）。
 *   - cmap 段直接复用 desc 里的指针，按 lv_font_fmt_txt_cmap_t 字段填充。
 *   - glyf 段不能零拷贝：v9 load_glyph 按位宽解码出 glyph_dsc[]，并把每个 glyph
 *     的位图重打包成连续段。所以这里要分配 glyph_dsc[] 与 glyph_bitmap。
 *   - kern 段：tables_count>=4 时解码。
 *
 * 回调：用 LVGL 9 内置 lv_font_get_glyph_dsc_fmt_txt / lv_font_get_bitmap_fmt_txt。
 */
#include "bin_font_lvgl_adapter.h"

#include "bin_font_probe.h"
#include "lvgl.h"
#ifdef ESP_PLATFORM
    #include "esp_heap_caps.h"
    #include "esp_log.h"
#endif
#include <string.h>

#ifdef ESP_PLATFORM
static const char *TAG = "bin_font";

static void log_alloc_failed(const char *what, size_t size)
{
    ESP_LOGE(TAG,
             "alloc %s failed: size=%lu free_8bit=%lu free_psram=%lu",
             what,
             (unsigned long) size,
             (unsigned long) heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned long) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void log_parse_failed(const char *stage)
{
    ESP_LOGE(TAG, "parse failed at %s", stage);
}
#else
static void log_alloc_failed(const char *what, size_t size)
{
    (void) what;
    (void) size;
}

static void log_parse_failed(const char *stage)
{
    (void) stage;
}
#endif

/* ------------------------------------------------------------------ */
/* 内存读助手（小端）                                                   */
/* ------------------------------------------------------------------ */

static uint32_t rd_u32m(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t rd_u16m(const uint8_t *p)
{
    return (uint16_t) (p[0] | (p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* 位迭代器（移植 v9 lv_binfont_loader.c）                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    const uint8_t *base;
    size_t         pos;
    int8_t         bit_pos;
    uint8_t        byte_value;
} mem_bit_iterator_t;

static mem_bit_iterator_t mem_bit_iter(const uint8_t *base)
{
    mem_bit_iterator_t it;
    it.base       = base;
    it.pos        = 0;
    it.bit_pos    = -1;
    it.byte_value = 0;
    return it;
}

static unsigned int mem_read_bits(mem_bit_iterator_t *it, int n_bits)
{
    unsigned int value = 0;
    while (n_bits--)
    {
        it->byte_value = (uint8_t) (it->byte_value << 1);
        it->bit_pos--;
        if (it->bit_pos < 0)
        {
            it->bit_pos    = 7;
            it->byte_value = it->base[it->pos++];
        }
        int8_t bit = (it->byte_value & 0x80) ? 1 : 0;
        value |= ((unsigned int) bit << n_bits);
    }
    return value;
}

static int mem_read_bits_signed(mem_bit_iterator_t *it, int n_bits)
{
    unsigned int value = mem_read_bits(it, n_bits);
    if (value & ((unsigned int) 1 << (n_bits - 1)))
    {
        value |= ~0u << n_bits;
    }
    return (int) value;
}

/* ------------------------------------------------------------------ */
/* load_glyph：解码 loca + glyf -> glyph_dsc[] + 连续 glyph_bitmap     */
/* ------------------------------------------------------------------ */

static bool load_glyph(const bin_font_desc_t *dsc, lv_font_fmt_txt_dsc_t *font_dsc)
{
    uint32_t loca_count                    = dsc->loca_count;

    lv_font_fmt_txt_glyph_dsc_t *glyph_dsc = lv_malloc(loca_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));
    if (glyph_dsc == NULL)
    {
        log_alloc_failed("glyph_dsc", loca_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));
        return false;
    }
    memset(glyph_dsc, 0, loca_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));
    font_dsc->glyph_dsc    = glyph_dsc;

    /* 解码 loca 表得到 uint32 glyph_offset[] */
    uint32_t *glyph_offset = lv_malloc(sizeof(uint32_t) * loca_count);
    if (glyph_offset == NULL)
    {
        log_alloc_failed("glyph_offset", sizeof(uint32_t) * loca_count);
        return false;
    }
    if (dsc->index_to_loc_format == 1)
    {
        const uint32_t *src = (const uint32_t *) dsc->loca_table;
        for (uint32_t i = 0; i < loca_count; i++)
        {
            glyph_offset[i] = rd_u32m((const uint8_t *) &src[i]);
        }
    }
    else
    {
        const uint16_t *src = (const uint16_t *) dsc->loca_table;
        for (uint32_t i = 0; i < loca_count; i++)
        {
            glyph_offset[i] = rd_u16m((const uint8_t *) &src[i]);
        }
    }

    /* 第一遍：解码每个 glyph 的字段，累计位图总大小 */
    int cur_bmp_size = 0;
    int nbits        = dsc->advance_width_bits + 2 * dsc->xy_bits + 2 * dsc->wh_bits;

    for (uint32_t i = 0; i < loca_count; i++)
    {
        lv_font_fmt_txt_glyph_dsc_t *gdsc = &glyph_dsc[i];
        const uint8_t               *gptr = dsc->glyf_body + glyph_offset[i];
        mem_bit_iterator_t           it   = mem_bit_iter(gptr);

        if (dsc->advance_width_bits == 0)
        {
            gdsc->adv_w = dsc->default_advance_width;
        }
        else
        {
            gdsc->adv_w = (uint16_t) mem_read_bits(&it, dsc->advance_width_bits);
        }
        if (dsc->advance_width_format == 0)
        {
            gdsc->adv_w = (uint16_t) (gdsc->adv_w * 16);
        }
        gdsc->ofs_x     = (int8_t) mem_read_bits_signed(&it, dsc->xy_bits);
        gdsc->ofs_y     = (int8_t) mem_read_bits_signed(&it, dsc->xy_bits);
        gdsc->box_w     = (uint8_t) mem_read_bits(&it, dsc->wh_bits);
        gdsc->box_h     = (uint8_t) mem_read_bits(&it, dsc->wh_bits);

        int next_offset = (i < loca_count - 1) ? (int) glyph_offset[i + 1] : (int) dsc->glyf_body_len;
        int bmp_size    = next_offset - (int) glyph_offset[i] - nbits / 8;

        if (i == 0)
        {
            /* .notdef 占位 */
            gdsc->adv_w = 0;
            gdsc->box_w = 0;
            gdsc->box_h = 0;
            gdsc->ofs_x = 0;
            gdsc->ofs_y = 0;
        }
        gdsc->bitmap_index = (uint32_t) cur_bmp_size;
        if ((int) gdsc->box_w * (int) gdsc->box_h != 0)
        {
            cur_bmp_size += bmp_size;
        }
    }

    /* 分配连续 glyph_bitmap */
    uint8_t *glyph_bmp = lv_malloc((size_t) cur_bmp_size);
    if (glyph_bmp == NULL)
    {
        log_alloc_failed("glyph_bitmap", (size_t) cur_bmp_size);
        lv_free(glyph_offset);
        return false;
    }
    font_dsc->glyph_bitmap = glyph_bmp;

    /* 第二遍：拷贝每个 glyph 的位图到连续段 */
    cur_bmp_size           = 0;
    for (uint32_t i = 1; i < loca_count; i++)
    {
        const uint8_t     *gptr = dsc->glyf_body + glyph_offset[i];
        mem_bit_iterator_t it   = mem_bit_iter(gptr);
        mem_read_bits(&it, nbits); /* 跳过 glyph 头部位 */

        if ((int) glyph_dsc[i].box_w * (int) glyph_dsc[i].box_h == 0)
        {
            continue;
        }

        int next_offset = (i < loca_count - 1) ? (int) glyph_offset[i + 1] : (int) dsc->glyf_body_len;
        int bmp_size    = next_offset - (int) glyph_offset[i] - nbits / 8;

        if (nbits % 8 == 0)
        {
            memcpy(&glyph_bmp[cur_bmp_size], gptr + nbits / 8, (size_t) bmp_size);
        }
        else
        {
            for (int k = 0; k < bmp_size - 1; k++)
            {
                glyph_bmp[cur_bmp_size + k] = (uint8_t) mem_read_bits(&it, 8);
            }
            glyph_bmp[cur_bmp_size + bmp_size - 1] = (uint8_t) mem_read_bits(&it, 8 - nbits % 8);
            glyph_bmp[cur_bmp_size + bmp_size - 1] = (uint8_t) (glyph_bmp[cur_bmp_size + bmp_size - 1] << (nbits % 8));
        }
        cur_bmp_size += bmp_size;
    }

    lv_free(glyph_offset);
    return true;
}

static void free_loaded_font_dsc(lv_font_fmt_txt_dsc_t *font_dsc)
{
    if (font_dsc == NULL)
    {
        return;
    }

    if (font_dsc->kern_dsc != NULL)
    {
        if (font_dsc->kern_classes)
        {
            lv_font_fmt_txt_kern_classes_t *kc = (lv_font_fmt_txt_kern_classes_t *) font_dsc->kern_dsc;
            lv_free((void *) kc->left_class_mapping);
            lv_free((void *) kc->right_class_mapping);
            lv_free((void *) kc->class_pair_values);
            lv_free(kc);
        }
        else
        {
            lv_font_fmt_txt_kern_pair_t *kp = (lv_font_fmt_txt_kern_pair_t *) font_dsc->kern_dsc;
            lv_free((void *) kp->glyph_ids);
            lv_free((void *) kp->values);
            lv_free(kp);
        }
    }

    lv_free((void *) font_dsc->cmaps);
    lv_free((void *) font_dsc->glyph_dsc);
    lv_free((void *) font_dsc->glyph_bitmap);
    lv_free(font_dsc);
}

void bin_font_lvgl_adapter_free(lv_font_t *font)
{
    if (font == NULL || font->dsc == NULL)
    {
        return;
    }
    free_loaded_font_dsc((lv_font_fmt_txt_dsc_t *) font->dsc);
    memset(font, 0, sizeof(*font));
}

/* ------------------------------------------------------------------ */
/* load_cmaps                                                          */
/* ------------------------------------------------------------------ */

static bool load_cmaps(const bin_font_desc_t *dsc, lv_font_fmt_txt_dsc_t *font_dsc)
{
    uint16_t                cmap_num = dsc->cmap_num;
    lv_font_fmt_txt_cmap_t *cmaps    = lv_malloc((size_t) cmap_num * sizeof(lv_font_fmt_txt_cmap_t));
    if (cmaps == NULL)
    {
        log_alloc_failed("cmaps", (size_t) cmap_num * sizeof(lv_font_fmt_txt_cmap_t));
        return false;
    }
    memset(cmaps, 0, (size_t) cmap_num * sizeof(lv_font_fmt_txt_cmap_t));
    font_dsc->cmaps    = cmaps;
    font_dsc->cmap_num = cmap_num;

    for (uint32_t i = 0; i < cmap_num; i++)
    {
        const bin_font_cmap_t  *c   = &dsc->cmaps[i];
        lv_font_fmt_txt_cmap_t *out = &cmaps[i];
        out->range_start            = c->range_start;
        out->range_length           = c->range_length;
        out->glyph_id_start         = c->glyph_id_start;
        out->unicode_list           = c->unicode_list;
        out->glyph_id_ofs_list      = c->glyph_id_ofs_list;
        out->list_length            = c->list_length;
        out->type                   = (lv_font_fmt_txt_cmap_type_t) c->format_type;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* load_kern（仅 tables_count>=4 时调用）                               */
/* ------------------------------------------------------------------ */

static bool load_kern(const bin_font_desc_t *dsc, lv_font_fmt_txt_dsc_t *font_dsc, uint8_t glyph_id_format)
{
    const uint8_t *p   = dsc->kern_body;
    const uint8_t *end = dsc->kern_body + dsc->kern_body_len;
    if (p + 4 > end)
    {
        return false;
    }
    uint8_t kern_format_type = p[0];
    p += 4;

    if (kern_format_type == 0)
    { /* sorted pairs */
        lv_font_fmt_txt_kern_pair_t *kp = lv_malloc(sizeof(lv_font_fmt_txt_kern_pair_t));
        if (kp == NULL)
        {
            log_alloc_failed("kern_pair", sizeof(lv_font_fmt_txt_kern_pair_t));
            return false;
        }
        memset(kp, 0, sizeof(*kp));
        font_dsc->kern_dsc     = kp;
        font_dsc->kern_classes = 0;

        if (p + 4 > end)
        {
            return false;
        }
        uint32_t glyph_entries = rd_u32m(p);
        p += 4;
        size_t ids_size =
            (glyph_id_format == 0) ? sizeof(int8_t) * 2 * glyph_entries : sizeof(int16_t) * 2 * glyph_entries;

        void   *glyph_ids = lv_malloc(ids_size);
        int8_t *values    = lv_malloc(glyph_entries);
        if (glyph_ids == NULL || values == NULL)
        {
            if (glyph_ids == NULL)
            {
                log_alloc_failed("kern_pair_ids", ids_size);
            }
            if (values == NULL)
            {
                log_alloc_failed("kern_pair_values", glyph_entries);
            }
            lv_free(glyph_ids);
            lv_free(values);
            return false;
        }

        kp->glyph_ids_size = glyph_id_format;
        kp->pair_cnt       = glyph_entries;
        kp->glyph_ids      = glyph_ids;
        kp->values         = values;

        if (p + ids_size > end)
        {
            return false;
        }
        memcpy(glyph_ids, p, ids_size);
        p += ids_size;
        if (p + glyph_entries > end)
        {
            return false;
        }
        memcpy(values, p, glyph_entries);
        return true;
    }
    else if (kern_format_type == 3)
    { /* M*N class array */
        lv_font_fmt_txt_kern_classes_t *kc = lv_malloc(sizeof(lv_font_fmt_txt_kern_classes_t));
        if (kc == NULL)
        {
            log_alloc_failed("kern_classes", sizeof(lv_font_fmt_txt_kern_classes_t));
            return false;
        }
        memset(kc, 0, sizeof(*kc));
        font_dsc->kern_dsc     = kc;
        font_dsc->kern_classes = 1;

        if (p + 4 > end)
        {
            return false;
        }
        uint16_t mapping_len = rd_u16m(p);
        uint8_t  rows        = p[2];
        uint8_t  cols        = p[3];
        p += 4;

        size_t   values_len = (size_t) sizeof(int8_t) * rows * cols;
        uint8_t *kleft      = lv_malloc(mapping_len);
        uint8_t *kright     = lv_malloc(mapping_len);
        int8_t  *kvals      = lv_malloc(values_len);
        if (kleft == NULL || kright == NULL || kvals == NULL)
        {
            if (kleft == NULL)
            {
                log_alloc_failed("kern_left", mapping_len);
            }
            if (kright == NULL)
            {
                log_alloc_failed("kern_right", mapping_len);
            }
            if (kvals == NULL)
            {
                log_alloc_failed("kern_values", values_len);
            }
            lv_free(kleft);
            lv_free(kright);
            lv_free(kvals);
            return false;
        }

        kc->left_class_mapping  = kleft;
        kc->right_class_mapping = kright;
        kc->left_class_cnt      = rows;
        kc->right_class_cnt     = cols;
        kc->class_pair_values   = kvals;

        if (p + mapping_len * 2 + values_len > end)
        {
            return false;
        }
        memcpy(kleft, p, mapping_len);
        p += mapping_len;
        memcpy(kright, p, mapping_len);
        p += mapping_len;
        memcpy(kvals, p, values_len);
        return true;
    }
    /* 未知 kern 格式：保守起见视为无 kern */
    font_dsc->kern_dsc     = NULL;
    font_dsc->kern_classes = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

bool bin_font_lvgl_adapter_build(const uint8_t *data, size_t len, lv_font_t *out)
{
    if (data == NULL || out == NULL)
    {
        return false;
    }

    bin_font_desc_t dsc;
    if (!bin_font_probe(data, len, &dsc))
    {
        log_parse_failed("probe");
        return false;
    }

    lv_font_fmt_txt_dsc_t *font_dsc = lv_malloc(sizeof(lv_font_fmt_txt_dsc_t));
    if (font_dsc == NULL)
    {
        log_alloc_failed("font_dsc", sizeof(lv_font_fmt_txt_dsc_t));
        return false;
    }
    memset(font_dsc, 0, sizeof(*font_dsc));

    lv_font_t parsed;
    memset(&parsed, 0, sizeof(parsed));

    /* head 字段 -> lv_font_t / lv_font_fmt_txt_dsc_t */
    parsed.base_line           = -dsc.descent;
    parsed.line_height         = dsc.ascent - dsc.descent;
    parsed.get_glyph_dsc       = lv_font_get_glyph_dsc_fmt_txt;
    parsed.get_glyph_bitmap    = lv_font_get_bitmap_fmt_txt;
    parsed.subpx               = dsc.subpixels_mode;
    parsed.underline_position  = (int8_t) dsc.underline_position;
    parsed.underline_thickness = (int8_t) dsc.underline_thickness;

    font_dsc->bpp              = dsc.bits_per_pixel;
    font_dsc->kern_scale       = dsc.kerning_scale;
    font_dsc->bitmap_format    = dsc.bitmap_format;

    /* cmap */
    if (!load_cmaps(&dsc, font_dsc))
    {
        log_parse_failed("cmap");
        free_loaded_font_dsc(font_dsc);
        return false;
    }

    /* glyph */
    if (!load_glyph(&dsc, font_dsc))
    {
        log_parse_failed("glyph");
        free_loaded_font_dsc(font_dsc);
        return false;
    }

    /* kern（可选） */
    if (dsc.tables_count >= 4)
    {
        if (!load_kern(&dsc, font_dsc, dsc.glyph_id_format))
        {
            log_parse_failed("kern");
            free_loaded_font_dsc(font_dsc);
            return false;
        }
    }
    else
    {
        font_dsc->kern_dsc     = NULL;
        font_dsc->kern_classes = 0;
        font_dsc->kern_scale   = 0;
    }

    parsed.dsc = font_dsc;
    *out       = parsed;
    return true;
}
