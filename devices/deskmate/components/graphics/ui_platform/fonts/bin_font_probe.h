/**
 * @file bin_font_probe.h
 *
 * 纯 C、零 LVGL 依赖的 LVGL bin 字体格式探针。
 *
 * 解析 lv_font_conv --format bin 生成的二进制字体，把构造 lv_font_t 所需的
 * 全部字段填入 bin_font_desc_t。所有指针直接指向入参 data 区间（不拷贝），
 * 因此 data 必须在 desc 使用期内常驻（典型场景：mmap 到 SPI flash 的只读区）。
 *
 * 移植自 LVGL 9 的 src/font/lv_binfont_loader.c（MIT 许可，版权归 LVGL authors）。
 */
#ifndef BIN_FONT_PROBE_H
#define BIN_FONT_PROBE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define BIN_FONT_MAX_CMAPS 64U

    /** 单个 cmap 子表的解析结果。指针均指向 data 区间。 */
    typedef struct
    {
        uint32_t        range_start;
        uint16_t        range_length;
        uint16_t        glyph_id_start;
        uint16_t        list_length;       /**< unicode_list / glyph_id_ofs_list 长度 */
        uint8_t         format_type;       /**< lv_font_fmt_txt_cmap_type_t */
        const uint16_t *unicode_list;      /**< 可为 NULL（format0 系列） */
        const void     *glyph_id_ofs_list; /**< uint8_t* 或 uint16_t*，可为 NULL */
    } bin_font_cmap_t;

    /** bin 字体解析的中间结果。指针字段均指向 data 区间（零拷贝）。 */
    typedef struct
    {
        /* ---- 来自 head 块的标量 ---- */
        uint16_t font_size;
        uint16_t ascent;
        int16_t  descent;
        uint16_t default_advance_width;
        uint16_t kerning_scale;
        uint8_t  bits_per_pixel;
        uint8_t  subpixels_mode;
        uint8_t  bitmap_format; /**< compression_id */
        int16_t  underline_position;
        uint16_t underline_thickness;

        /* ---- head 块里仅 load_glyph 解码需要的位宽参数 ---- */
        uint8_t index_to_loc_format; /**< 0: loca 为 uint16_t，1: uint32_t */
        uint8_t glyph_id_format;     /**< 0: kern glyph_ids 为 uint8，1: uint16 */
        uint8_t advance_width_bits;
        uint8_t advance_width_format; /**< 0: adv_w 需 *16 */
        uint8_t xy_bits;
        uint8_t wh_bits;

        /* ---- tables_count：v9 源码里 <4 表示无 kern 块 ---- */
        uint16_t tables_count;

        /* ---- cmap 段 ---- */
        bin_font_cmap_t cmaps[BIN_FONT_MAX_CMAPS];
        uint16_t        cmap_num;

        /* ---- loca 段 ---- */
        const void *loca_table;
        uint32_t    loca_count;

        /* ---- glyf 段 ---- */
        const uint8_t *glyf_body;
        uint32_t       glyf_body_len;

        /* ---- kern 段（可选，tables_count>=4 才存在） ---- */
        const uint8_t *kern_body;
        uint32_t       kern_body_len;
    } bin_font_desc_t;

    /**
 * 从内存中的 LVGL bin 字体数据解析，填充 out（零拷贝：指针字段指向 data）。
 * @param data  lv_font_conv --format bin 生成的字体数据
 * @param len   data 字节数
 * @param out   输出（调用方分配）
 * @return true 解析成功；false 数据非法/截断/越界
 */
    bool bin_font_probe(const uint8_t *data, size_t len, bin_font_desc_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BIN_FONT_PROBE_H */
