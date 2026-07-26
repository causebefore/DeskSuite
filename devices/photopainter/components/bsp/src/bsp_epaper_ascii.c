/**
 * @file bsp_epaper_ascii.c
 * @brief 实现墨水屏 1 bpp/2 bpp 帧内的紧凑 ASCII 状态文本合成
 */
#include "bsp.h"

#include <string.h>

/** @brief 5x7 大写英文字模，每行低 5 位从左到右表示像素 */
static const uint8_t BSP_EPAPER_ASCII_UPPERCASE[26][7] = {
    { 0x0EU, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U },
    { 0x1EU, 0x11U, 0x11U, 0x1EU, 0x11U, 0x11U, 0x1EU },
    { 0x0EU, 0x11U, 0x10U, 0x10U, 0x10U, 0x11U, 0x0EU },
    { 0x1EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x1EU },
    { 0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x1FU },
    { 0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x10U },
    { 0x0EU, 0x11U, 0x10U, 0x17U, 0x11U, 0x11U, 0x0FU },
    { 0x11U, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U },
    { 0x0EU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU },
    { 0x07U, 0x02U, 0x02U, 0x02U, 0x12U, 0x12U, 0x0CU },
    { 0x11U, 0x12U, 0x14U, 0x18U, 0x14U, 0x12U, 0x11U },
    { 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x1FU },
    { 0x11U, 0x1BU, 0x15U, 0x15U, 0x11U, 0x11U, 0x11U },
    { 0x11U, 0x19U, 0x15U, 0x13U, 0x11U, 0x11U, 0x11U },
    { 0x0EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU },
    { 0x1EU, 0x11U, 0x11U, 0x1EU, 0x10U, 0x10U, 0x10U },
    { 0x0EU, 0x11U, 0x11U, 0x11U, 0x15U, 0x12U, 0x0DU },
    { 0x1EU, 0x11U, 0x11U, 0x1EU, 0x14U, 0x12U, 0x11U },
    { 0x0FU, 0x10U, 0x10U, 0x0EU, 0x01U, 0x01U, 0x1EU },
    { 0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U },
    { 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU },
    { 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0AU, 0x04U },
    { 0x11U, 0x11U, 0x11U, 0x15U, 0x15U, 0x15U, 0x0AU },
    { 0x11U, 0x11U, 0x0AU, 0x04U, 0x0AU, 0x11U, 0x11U },
    { 0x11U, 0x11U, 0x0AU, 0x04U, 0x04U, 0x04U, 0x04U },
    { 0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x1FU },
};

/** @brief 5x7 小写英文字模 */
static const uint8_t BSP_EPAPER_ASCII_LOWERCASE[26][7] = {
    { 0x00U, 0x00U, 0x0EU, 0x01U, 0x0FU, 0x11U, 0x0FU },
    { 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x11U, 0x1EU },
    { 0x00U, 0x00U, 0x0FU, 0x10U, 0x10U, 0x10U, 0x0FU },
    { 0x01U, 0x01U, 0x0FU, 0x11U, 0x11U, 0x11U, 0x0FU },
    { 0x00U, 0x00U, 0x0EU, 0x11U, 0x1FU, 0x10U, 0x0EU },
    { 0x06U, 0x09U, 0x08U, 0x1CU, 0x08U, 0x08U, 0x08U },
    { 0x00U, 0x00U, 0x0FU, 0x11U, 0x0FU, 0x01U, 0x0EU },
    { 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x11U, 0x11U },
    { 0x04U, 0x00U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x0EU },
    { 0x02U, 0x00U, 0x06U, 0x02U, 0x02U, 0x12U, 0x0CU },
    { 0x10U, 0x10U, 0x12U, 0x14U, 0x18U, 0x14U, 0x12U },
    { 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU },
    { 0x00U, 0x00U, 0x1AU, 0x15U, 0x15U, 0x15U, 0x15U },
    { 0x00U, 0x00U, 0x1EU, 0x11U, 0x11U, 0x11U, 0x11U },
    { 0x00U, 0x00U, 0x0EU, 0x11U, 0x11U, 0x11U, 0x0EU },
    { 0x00U, 0x00U, 0x1EU, 0x11U, 0x1EU, 0x10U, 0x10U },
    { 0x00U, 0x00U, 0x0FU, 0x11U, 0x0FU, 0x01U, 0x01U },
    { 0x00U, 0x00U, 0x16U, 0x19U, 0x10U, 0x10U, 0x10U },
    { 0x00U, 0x00U, 0x0FU, 0x10U, 0x0EU, 0x01U, 0x1EU },
    { 0x08U, 0x08U, 0x1CU, 0x08U, 0x08U, 0x09U, 0x06U },
    { 0x00U, 0x00U, 0x11U, 0x11U, 0x11U, 0x13U, 0x0DU },
    { 0x00U, 0x00U, 0x11U, 0x11U, 0x11U, 0x0AU, 0x04U },
    { 0x00U, 0x00U, 0x11U, 0x11U, 0x15U, 0x15U, 0x0AU },
    { 0x00U, 0x00U, 0x11U, 0x0AU, 0x04U, 0x0AU, 0x11U },
    { 0x00U, 0x00U, 0x11U, 0x11U, 0x0FU, 0x01U, 0x0EU },
    { 0x00U, 0x00U, 0x1FU, 0x02U, 0x04U, 0x08U, 0x1FU },
};

/** @brief 5x7 数字字模 */
static const uint8_t BSP_EPAPER_ASCII_DIGITS[10][7] = {
    { 0x0EU, 0x11U, 0x13U, 0x15U, 0x19U, 0x11U, 0x0EU },
    { 0x04U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU },
    { 0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x08U, 0x1FU },
    { 0x1EU, 0x01U, 0x01U, 0x0EU, 0x01U, 0x01U, 0x1EU },
    { 0x02U, 0x06U, 0x0AU, 0x12U, 0x1FU, 0x02U, 0x02U },
    { 0x1FU, 0x10U, 0x10U, 0x1EU, 0x01U, 0x01U, 0x1EU },
    { 0x0EU, 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x0EU },
    { 0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x08U, 0x08U },
    { 0x0EU, 0x11U, 0x11U, 0x0EU, 0x11U, 0x11U, 0x0EU },
    { 0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x01U, 0x0EU },
};

/**
 * @brief 查询一个受支持 ASCII 字符的 5x7 字模
 *
 * @param[in] character ASCII 字符
 * @param[out] out_rows 返回七行字模的只读指针
 * @return true 字符受支持；false 字符不受支持
 */
static bool bsp_epaper_ascii_glyph(char character, const uint8_t **out_rows)
{
    static const uint8_t space[7] = { 0 };
    static const uint8_t exclamation[7] = { 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x00U, 0x04U };
    static const uint8_t hyphen[7] = { 0x00U, 0x00U, 0x00U, 0x1FU, 0x00U, 0x00U, 0x00U };
    static const uint8_t period[7] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U };
    static const uint8_t colon[7] = { 0x00U, 0x04U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U };
    static const uint8_t question[7] = { 0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x00U, 0x04U };
    static const uint8_t underscore[7] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x1FU };

    if (character >= 'A' && character <= 'Z')
    {
        *out_rows = BSP_EPAPER_ASCII_UPPERCASE[(size_t) (character - 'A')];
        return true;
    }
    if (character >= 'a' && character <= 'z')
    {
        *out_rows = BSP_EPAPER_ASCII_LOWERCASE[(size_t) (character - 'a')];
        return true;
    }
    if (character >= '0' && character <= '9')
    {
        *out_rows = BSP_EPAPER_ASCII_DIGITS[(size_t) (character - '0')];
        return true;
    }
    switch (character)
    {
        case ' ': *out_rows = space; break;
        case '!': *out_rows = exclamation; break;
        case '-': *out_rows = hyphen; break;
        case '.': *out_rows = period; break;
        case ':': *out_rows = colon; break;
        case '?': *out_rows = question; break;
        case '_': *out_rows = underscore; break;
        default: return false;
    }
    return true;
}

/** @brief 校验可变帧的尺寸、步长和缓冲区范围 */
static bool bsp_epaper_ascii_frame_is_valid(const bsp_epaper_frame_view_t *frame)
{
    if (frame == NULL || frame->pixels == NULL || frame->width_pixels == 0U
        || frame->height_pixels == 0U || frame->format < BSP_EPAPER_FRAME_MONO_1BPP
        || frame->format > BSP_EPAPER_FRAME_GRAY_2BPP)
    {
        return false;
    }
    const size_t bits_per_pixel = frame->format == BSP_EPAPER_FRAME_MONO_1BPP ? 1U : 2U;
    const size_t minimum_stride = (((size_t) frame->width_pixels * bits_per_pixel) + 7U) / 8U;
    return frame->stride_bytes >= minimum_stride
           && frame->height_pixels <= SIZE_MAX / frame->stride_bytes
           && frame->size_bytes >= frame->stride_bytes * frame->height_pixels;
}

/** @brief 把一个黑色像素写入可变的 1 bpp 或 2 bpp 帧 */
static void bsp_epaper_ascii_set_black(bsp_epaper_frame_view_t *frame,
                                       uint16_t x_pixels,
                                       uint16_t y_pixels)
{
    uint8_t *row = frame->pixels + ((size_t) y_pixels * frame->stride_bytes);
    if (frame->format == BSP_EPAPER_FRAME_MONO_1BPP)
    {
        row[x_pixels / 8U] |= (uint8_t) (0x80U >> (x_pixels % 8U));
        return;
    }
    const uint8_t shift = (uint8_t) ((3U - (x_pixels % 4U)) * 2U);
    row[x_pixels / 4U] &= (uint8_t) ~(0x03U << shift);
}

esp_err_t bsp_epaper_measure_ascii_copy(const char *text, uint8_t scale,
                                        bsp_epaper_ascii_size_t *out_size)
{
    if (text == NULL || scale == 0U || out_size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    size_t text_length = 0U;
    while (text_length <= BSP_EPAPER_ASCII_TEXT_MAX && text[text_length] != '\0')
    {
        const uint8_t *glyph;
        if (!bsp_epaper_ascii_glyph(text[text_length], &glyph))
        {
            return ESP_ERR_INVALID_ARG;
        }
        ++text_length;
    }
    if (text_length == 0U || text_length > BSP_EPAPER_ASCII_TEXT_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t logical_width = (text_length * 6U) - 1U;
    out_size->width_pixels = (uint16_t) (logical_width * scale);
    out_size->height_pixels = (uint16_t) (7U * scale);
    return ESP_OK;
}

esp_err_t bsp_epaper_draw_ascii_borrow(bsp_epaper_frame_view_t *frame, uint16_t x_pixels,
                                       uint16_t y_pixels, const char *text, uint8_t scale)
{
    if (!bsp_epaper_ascii_frame_is_valid(frame))
    {
        return ESP_ERR_INVALID_ARG;
    }
    bsp_epaper_ascii_size_t text_size;
    const esp_err_t measure_error = bsp_epaper_measure_ascii_copy(text, scale, &text_size);
    if (measure_error != ESP_OK)
    {
        return measure_error;
    }
    if (x_pixels >= frame->width_pixels || y_pixels >= frame->height_pixels
        || text_size.width_pixels > frame->width_pixels - x_pixels
        || text_size.height_pixels > frame->height_pixels - y_pixels)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t text_length = (text_size.width_pixels / scale + 1U) / 6U;
    for (size_t character_index = 0U; character_index < text_length; ++character_index)
    {
        const uint8_t *glyph;
        (void) bsp_epaper_ascii_glyph(text[character_index], &glyph);
        for (uint8_t glyph_y = 0U; glyph_y < 7U; ++glyph_y)
        {
            for (uint8_t glyph_x = 0U; glyph_x < 5U; ++glyph_x)
            {
                if ((glyph[glyph_y] & (uint8_t) (0x10U >> glyph_x)) == 0U)
                {
                    continue;
                }
                const uint16_t pixel_x = (uint16_t) (x_pixels
                    + ((character_index * 6U + glyph_x) * scale));
                const uint16_t pixel_y = (uint16_t) (y_pixels + (glyph_y * scale));
                for (uint8_t scale_y = 0U; scale_y < scale; ++scale_y)
                {
                    for (uint8_t scale_x = 0U; scale_x < scale; ++scale_x)
                    {
                        bsp_epaper_ascii_set_black(frame,
                                                   (uint16_t) (pixel_x + scale_x),
                                                   (uint16_t) (pixel_y + scale_y));
                    }
                }
            }
        }
    }
    return ESP_OK;
}
