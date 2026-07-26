/**
 * @file device_display.c
 * @brief 实现拥有内部单帧的设备级同步显示能力
 */
#include "device_display.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define DEVICE_DISPLAY_WIDTH_PIXELS  800U
#define DEVICE_DISPLAY_HEIGHT_PIXELS 480U
#define DEVICE_DISPLAY_MONO_FRAME_SIZE_BYTES \
    ((DEVICE_DISPLAY_WIDTH_PIXELS * DEVICE_DISPLAY_HEIGHT_PIXELS) / 8U)
#define DEVICE_DISPLAY_GRAYSCALE_FRAME_SIZE_BYTES \
    ((DEVICE_DISPLAY_WIDTH_PIXELS * DEVICE_DISPLAY_HEIGHT_PIXELS) / 4U)

/** @brief 日志标签 */
static const char *TAG = "device_display";

/** @brief 显示能力是否已初始化 */
static bool s_initialized;

/** @brief 面板是否已进入深睡 */
static bool s_sleeping;

/** @brief 当前设备显示全局刷新模式 */
static device_display_mode_t s_mode;

/** @brief 由 Device 独占的内部单帧 */
static uint8_t *s_frame;

/** @brief 当前内部帧的有效字节数 */
static size_t s_frame_size_bytes;

/** @brief 判断色调枚举是否有效 */
static bool device_display_is_valid_tone(device_display_tone_t tone)
{
    return tone >= DEVICE_DISPLAY_TONE_BLACK && tone <= DEVICE_DISPLAY_TONE_WHITE;
}

/** @brief 根据当前模式把一个语义色调写入内部帧指定像素 */
static void device_display_set_pixel(uint16_t x_pixels, uint16_t y_pixels,
                                     device_display_tone_t tone)
{
    if (s_mode == DEVICE_DISPLAY_MODE_MONOCHROME)
    {
        const size_t  row_size_bytes = DEVICE_DISPLAY_WIDTH_PIXELS / 8U;
        const size_t  byte_index     = ((size_t) y_pixels * row_size_bytes) + (x_pixels / 8U);
        const uint8_t mask           = (uint8_t) (0x80U >> (x_pixels % 8U));
        if (tone <= DEVICE_DISPLAY_TONE_DARK_GRAY)
        {
            s_frame[byte_index] |= mask;
        }
        else
        {
            s_frame[byte_index] &= (uint8_t) ~mask;
        }
        return;
    }

    const size_t  row_size_bytes = DEVICE_DISPLAY_WIDTH_PIXELS / 4U;
    const size_t  byte_index     = ((size_t) y_pixels * row_size_bytes) + (x_pixels / 4U);
    const uint8_t shift          = (uint8_t) ((3U - (x_pixels % 4U)) * 2U);
    const uint8_t mask           = (uint8_t) (0x03U << shift);
    s_frame[byte_index] =
        (uint8_t) ((s_frame[byte_index] & (uint8_t) ~mask) | ((uint8_t) tone << shift));
}

/**
 * @brief 校验图像视图并计算每行最少字节数
 *
 * @param[in] image 待校验图像
 * @return ESP_OK 有效；ESP_ERR_INVALID_ARG 格式、尺寸、步长或容量无效
 */
static esp_err_t device_display_validate_image(const device_display_image_view_t *image)
{
    if (image == NULL || image->pixels == NULL || image->width_pixels == 0U
        || image->height_pixels == 0U || image->pixel_format < DEVICE_DISPLAY_PIXEL_FORMAT_MONO_1BPP
        || image->pixel_format > DEVICE_DISPLAY_PIXEL_FORMAT_GRAY_8BPP)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bits_per_pixel;
    switch (image->pixel_format)
    {
        case DEVICE_DISPLAY_PIXEL_FORMAT_MONO_1BPP:
            bits_per_pixel = 1U;
            break;
        case DEVICE_DISPLAY_PIXEL_FORMAT_GRAY_2BPP:
            bits_per_pixel = 2U;
            break;
        case DEVICE_DISPLAY_PIXEL_FORMAT_GRAY_8BPP:
            bits_per_pixel = 8U;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    const size_t min_row_size_bytes = (((size_t) image->width_pixels * bits_per_pixel) + 7U) / 8U;
    if (image->stride_bytes < min_row_size_bytes)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t preceding_rows = (size_t) image->height_pixels - 1U;
    if (preceding_rows > 0U
        && image->stride_bytes > (SIZE_MAX - min_row_size_bytes) / preceding_rows)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t required_size_bytes = (preceding_rows * image->stride_bytes) + min_row_size_bytes;
    if (image->size_bytes < required_size_bytes)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/** @brief 从源图像的一行中读取一个像素并统一为四级语义色调 */
static device_display_tone_t
    device_display_read_source_tone(const uint8_t *row, uint16_t x_pixels,
                                    device_display_pixel_format_t pixel_format)
{
    if (pixel_format == DEVICE_DISPLAY_PIXEL_FORMAT_MONO_1BPP)
    {
        const uint8_t mask = (uint8_t) (0x80U >> (x_pixels % 8U));
        return (row[x_pixels / 8U] & mask) != 0U ? DEVICE_DISPLAY_TONE_BLACK
                                                 : DEVICE_DISPLAY_TONE_WHITE;
    }
    if (pixel_format == DEVICE_DISPLAY_PIXEL_FORMAT_GRAY_2BPP)
    {
        const uint8_t shift = (uint8_t) ((3U - (x_pixels % 4U)) * 2U);
        return (device_display_tone_t) ((row[x_pixels / 4U] >> shift) & 0x03U);
    }

    const uint16_t quantized = ((uint16_t) row[x_pixels] + 42U) / 85U;
    return (device_display_tone_t) (quantized > DEVICE_DISPLAY_TONE_WHITE
                                        ? DEVICE_DISPLAY_TONE_WHITE
                                        : quantized);
}

esp_err_t device_display_init(device_display_mode_t mode)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode < DEVICE_DISPLAY_MODE_MONOCHROME || mode > DEVICE_DISPLAY_MODE_GRAYSCALE_4)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t frame_size_bytes = mode == DEVICE_DISPLAY_MODE_GRAYSCALE_4
                                        ? DEVICE_DISPLAY_GRAYSCALE_FRAME_SIZE_BYTES
                                        : DEVICE_DISPLAY_MONO_FRAME_SIZE_BYTES;
    uint8_t     *frame = heap_caps_malloc(frame_size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool         frame_in_psram = frame != NULL;
    if (frame == NULL)
    {
        frame = heap_caps_malloc(frame_size_bytes, MALLOC_CAP_8BIT);
    }
    if (frame == NULL)
    {
        ESP_LOGE(TAG, "显示内部帧分配失败，需要 %u 字节", (unsigned) frame_size_bytes);
        return ESP_ERR_NO_MEM;
    }

    memset(frame, mode == DEVICE_DISPLAY_MODE_GRAYSCALE_4 ? 0xFF : 0x00, frame_size_bytes);

    const bsp_epaper_mode_t bsp_mode = mode == DEVICE_DISPLAY_MODE_GRAYSCALE_4
                                           ? BSP_EPAPER_MODE_GRAYSCALE_4
                                           : BSP_EPAPER_MODE_MONOCHROME;
    const esp_err_t         error    = bsp_epaper_init(bsp_mode);
    if (error != ESP_OK)
    {
        heap_caps_free(frame);
        return error;
    }

    s_frame            = frame;
    s_frame_size_bytes = frame_size_bytes;
    s_initialized      = true;
    s_sleeping         = false;
    s_mode             = mode;
    ESP_LOGI(TAG,
             "设备显示初始化完成，模式 %s，内部帧 %u 字节，位于%s",
             mode == DEVICE_DISPLAY_MODE_GRAYSCALE_4 ? "4 灰阶全刷" : "单色全刷",
             (unsigned) frame_size_bytes,
             frame_in_psram ? "PSRAM" : "内部内存");
    return ESP_OK;
}

esp_err_t device_display_get_info_copy(device_display_info_t *out_info)
{
    if (out_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    out_info->width_pixels  = DEVICE_DISPLAY_WIDTH_PIXELS;
    out_info->height_pixels = DEVICE_DISPLAY_HEIGHT_PIXELS;
    out_info->mode          = s_mode;
    return ESP_OK;
}

esp_err_t device_display_clear(device_display_tone_t tone)
{
    if (!device_display_is_valid_tone(tone))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mode == DEVICE_DISPLAY_MODE_MONOCHROME)
    {
        memset(s_frame, tone <= DEVICE_DISPLAY_TONE_DARK_GRAY ? 0xFF : 0x00, s_frame_size_bytes);
        return ESP_OK;
    }

    const uint8_t gray = (uint8_t) tone;
    const uint8_t fill = (uint8_t) ((gray << 6U) | (gray << 4U) | (gray << 2U) | gray);
    memset(s_frame, fill, s_frame_size_bytes);
    return ESP_OK;
}

esp_err_t device_display_fill_rect(uint16_t x_pixels, uint16_t y_pixels, uint16_t width_pixels,
                                   uint16_t height_pixels, device_display_tone_t tone)
{
    if (!device_display_is_valid_tone(tone) || width_pixels == 0U || height_pixels == 0U
        || x_pixels >= DEVICE_DISPLAY_WIDTH_PIXELS || y_pixels >= DEVICE_DISPLAY_HEIGHT_PIXELS
        || width_pixels > DEVICE_DISPLAY_WIDTH_PIXELS - x_pixels
        || height_pixels > DEVICE_DISPLAY_HEIGHT_PIXELS - y_pixels)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint16_t y = y_pixels; y < y_pixels + height_pixels; ++y)
    {
        for (uint16_t x = x_pixels; x < x_pixels + width_pixels; ++x)
        {
            device_display_set_pixel(x, y, tone);
        }
    }
    return ESP_OK;
}

esp_err_t device_display_blit_borrow(uint16_t x_pixels, uint16_t y_pixels,
                                     const device_display_image_view_t *image)
{
    esp_err_t error = device_display_validate_image(image);
    if (error != ESP_OK)
    {
        return error;
    }
    if (x_pixels >= DEVICE_DISPLAY_WIDTH_PIXELS || y_pixels >= DEVICE_DISPLAY_HEIGHT_PIXELS
        || image->width_pixels > DEVICE_DISPLAY_WIDTH_PIXELS - x_pixels
        || image->height_pixels > DEVICE_DISPLAY_HEIGHT_PIXELS - y_pixels)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t *pixels = image->pixels;
    for (uint16_t source_y = 0U; source_y < image->height_pixels; ++source_y)
    {
        const uint8_t *row = pixels + ((size_t) source_y * image->stride_bytes);
        for (uint16_t source_x = 0U; source_x < image->width_pixels; ++source_x)
        {
            const device_display_tone_t tone =
                device_display_read_source_tone(row, source_x, image->pixel_format);
            device_display_set_pixel((uint16_t) (x_pixels + source_x),
                                     (uint16_t) (y_pixels + source_y),
                                     tone);
        }
    }
    return ESP_OK;
}

esp_err_t device_display_measure_ascii_copy(const char *text, uint8_t scale,
                                            device_display_ascii_size_t *out_size)
{
    if (out_size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    bsp_epaper_ascii_size_t bsp_size;
    const esp_err_t error = bsp_epaper_measure_ascii_copy(text, scale, &bsp_size);
    if (error == ESP_OK)
    {
        out_size->width_pixels = bsp_size.width_pixels;
        out_size->height_pixels = bsp_size.height_pixels;
    }
    return error;
}

esp_err_t device_display_draw_ascii_borrow(uint16_t x_pixels, uint16_t y_pixels,
                                           const char *text, uint8_t scale)
{
    if (!s_initialized || s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }
    bsp_epaper_frame_view_t frame = {
        .pixels = s_frame,
        .size_bytes = s_frame_size_bytes,
        .stride_bytes = s_mode == DEVICE_DISPLAY_MODE_GRAYSCALE_4
                            ? DEVICE_DISPLAY_WIDTH_PIXELS / 4U
                            : DEVICE_DISPLAY_WIDTH_PIXELS / 8U,
        .width_pixels = DEVICE_DISPLAY_WIDTH_PIXELS,
        .height_pixels = DEVICE_DISPLAY_HEIGHT_PIXELS,
        .format = s_mode == DEVICE_DISPLAY_MODE_GRAYSCALE_4
                      ? BSP_EPAPER_FRAME_GRAY_2BPP
                      : BSP_EPAPER_FRAME_MONO_1BPP,
    };
    return bsp_epaper_draw_ascii_borrow(&frame, x_pixels, y_pixels, text, scale);
}

esp_err_t device_display_present_ascii_centered_borrow(const char *text, uint8_t scale)
{
    if (!s_initialized || s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }
    device_display_ascii_size_t text_size;
    ESP_RETURN_ON_ERROR(device_display_measure_ascii_copy(text, scale, &text_size),
                        TAG, "计算居中 ASCII 状态文本尺寸失败");
    if (text_size.width_pixels > DEVICE_DISPLAY_WIDTH_PIXELS
        || text_size.height_pixels > DEVICE_DISPLAY_HEIGHT_PIXELS)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(device_display_clear(DEVICE_DISPLAY_TONE_WHITE),
                        TAG, "清空居中 ASCII 状态页失败");
    const uint16_t x_pixels = (uint16_t) ((DEVICE_DISPLAY_WIDTH_PIXELS
                                           - text_size.width_pixels) / 2U);
    const uint16_t y_pixels = (uint16_t) ((DEVICE_DISPLAY_HEIGHT_PIXELS
                                           - text_size.height_pixels) / 2U);
    ESP_RETURN_ON_ERROR(device_display_draw_ascii_borrow(x_pixels, y_pixels, text, scale),
                        TAG, "合成居中 ASCII 状态文本失败");
    return device_display_present();
}

esp_err_t device_display_present(void)
{
    if (!s_initialized || s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mode == DEVICE_DISPLAY_MODE_GRAYSCALE_4)
    {
        return bsp_epaper_display_grayscale_borrow(s_frame, s_frame_size_bytes);
    }
    return bsp_epaper_display_monochrome_borrow(s_frame, s_frame_size_bytes);
}

esp_err_t device_display_sleep(void)
{
    if (!s_initialized || s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(bsp_epaper_sleep(), TAG, "设备显示进入深睡失败");
    s_sleeping = true;
    return ESP_OK;
}

esp_err_t device_display_deinit(void)
{
    if (!s_initialized || !s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(bsp_epaper_deinit(), TAG, "设备显示资源释放失败");
    heap_caps_free(s_frame);
    s_frame            = NULL;
    s_frame_size_bytes = 0U;
    s_initialized      = false;
    s_sleeping         = false;
    s_mode             = DEVICE_DISPLAY_MODE_MONOCHROME;
    return ESP_OK;
}
