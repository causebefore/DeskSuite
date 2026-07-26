/**
 * @file uc8179.c
 * @brief 实现 UC8179 控制器初始化、全屏填充和深睡时序
 */
#include "uc8179.h"

#include <string.h>

#include "utils.h"

#define UC8179_CMD_PANEL_SETTING          0x00U
#define UC8179_CMD_POWER_SETTING          0x01U
#define UC8179_CMD_POWER_OFF              0x02U
#define UC8179_CMD_POWER_ON               0x04U
#define UC8179_CMD_BOOSTER_SOFT_START     0x06U
#define UC8179_CMD_DEEP_SLEEP             0x07U
#define UC8179_CMD_DATA_START_OLD         0x10U
#define UC8179_CMD_DISPLAY_REFRESH        0x12U
#define UC8179_CMD_DATA_START_NEW         0x13U
#define UC8179_CMD_DUAL_SPI               0x15U
#define UC8179_CMD_PARTIAL_WINDOW         0x90U
#define UC8179_CMD_PARTIAL_IN             0x91U
#define UC8179_CMD_PARTIAL_OUT            0x92U
#define UC8179_CMD_VCOM_DATA_INTERVAL     0x50U
#define UC8179_CMD_TCON_SETTING           0x60U
#define UC8179_CMD_RESOLUTION_SETTING     0x61U
#define UC8179_CMD_CASCADE_SETTING        0xE0U
#define UC8179_CMD_FORCED_TEMPERATURE     0xE5U

#define UC8179_RESET_LOW_DELAY_MS         10U
#define UC8179_RESET_HIGH_DELAY_MS        10U
#define UC8179_POWER_ON_SETTLE_DELAY_MS   100U
#define UC8179_POWER_OFF_SETTLE_DELAY_MS  10U
#define UC8179_REFRESH_COMMAND_DELAY_MS   1U
#define UC8179_BUSY_POLL_PERIOD_MS        10U
#define UC8179_FILL_BLOCK_SIZE_BYTES      256U
#define UC8179_GRAYSCALE_BLOCK_SIZE_BYTES 256U
#define UC8179_DEEP_SLEEP_CHECK_CODE      0xA5U
#define UC8179_GRAYSCALE_CASCADE_SETTING  0x02U

/** @brief 同步发送一个命令及其连续参数 */
static esp_err_t uc8179_write_command_data(uc8179_t *controller, uint8_t command,
                                           const uint8_t *data, size_t size_bytes)
{
    esp_err_t error =
        controller->config.write(false, &command, sizeof(command), controller->config.io_context);
    if (error == ESP_OK && size_bytes > 0U)
    {
        error = controller->config.write(true, data, size_bytes, controller->config.io_context);
    }
    return error;
}

/** @brief 同步发送一个无参数命令 */
static esp_err_t uc8179_write_command(uc8179_t *controller, uint8_t command)
{
    return uc8179_write_command_data(controller, command, NULL, 0U);
}

/**
 * @brief 有界轮询 BUSY，直到控制器回到空闲
 *
 * initial_delay_ms 用于跨过命令发出后 BUSY 尚未拉低的短暂窗口，避免误判为已完成。
 */
static esp_err_t uc8179_wait_idle(uc8179_t *controller, uint32_t initial_delay_ms)
{
    uint32_t elapsed_ms = initial_delay_ms;
    controller->config.delay(initial_delay_ms, controller->config.io_context);

    while (true)
    {
        bool      busy;
        esp_err_t error = controller->config.read_busy(&busy, controller->config.io_context);
        if (error != ESP_OK)
        {
            return error;
        }
        if (!busy)
        {
            return ESP_OK;
        }
        if (elapsed_ms >= controller->config.busy_timeout_ms)
        {
            return ESP_ERR_TIMEOUT;
        }

        uint32_t delay_ms = UC8179_BUSY_POLL_PERIOD_MS;
        if (delay_ms > controller->config.busy_timeout_ms - elapsed_ms)
        {
            delay_ms = controller->config.busy_timeout_ms - elapsed_ms;
        }
        controller->config.delay(delay_ms, controller->config.io_context);
        elapsed_ms += delay_ms;
    }
}

/** @brief 以固定字节分块填充控制器 SRAM，避免分配整帧缓冲区 */
static esp_err_t uc8179_write_repeated_data(uc8179_t *controller, uint8_t value, size_t size_bytes)
{
    uint8_t block[UC8179_FILL_BLOCK_SIZE_BYTES];
    memset(block, value, sizeof(block));

    while (size_bytes > 0U)
    {
        const size_t chunk_size = size_bytes < sizeof(block) ? size_bytes : sizeof(block);
        esp_err_t    error =
            controller->config.write(true, block, chunk_size, controller->config.io_context);
        if (error != ESP_OK)
        {
            return error;
        }
        size_bytes -= chunk_size;
    }
    return ESP_OK;
}

/** @brief 写入面板分辨率，确保控制器 SRAM 窗口与物理面板一致 */
static esp_err_t uc8179_write_resolution(uc8179_t *controller)
{
    const uint8_t resolution[] = {
        (uint8_t) (controller->config.width_pixels >> 8U),
        (uint8_t) controller->config.width_pixels,
        (uint8_t) (controller->config.height_pixels >> 8U),
        (uint8_t) controller->config.height_pixels,
    };
    return uc8179_write_command_data(controller,
                                     UC8179_CMD_RESOLUTION_SETTING,
                                     resolution,
                                     sizeof(resolution));
}

/** @brief 把两个 2 bpp 输入字节编码为 UC8179 的一个灰阶位平面字节 */
static uint8_t uc8179_encode_grayscale_plane_byte(const uint8_t *pixels_2bpp, size_t output_index,
                                                  bool new_plane)
{
    uint8_t output = 0U;
    for (size_t source_index = 0U; source_index < 2U; ++source_index)
    {
        const uint8_t packed = pixels_2bpp[(output_index * 2U) + source_index];
        for (uint8_t pixel_index = 0U; pixel_index < 4U; ++pixel_index)
        {
            const uint8_t gray      = (packed >> (6U - (pixel_index * 2U))) & 0x03U;
            const uint8_t plane_bit = new_plane ? ((gray >> 1U) & 0x01U) : (gray & 0x01U);
            output                  = (uint8_t) ((output << 1U) | (plane_bit ^ 0x01U));
        }
    }
    return output;
}

/** @brief 在线转换并写入一张灰阶位平面，避免额外分配 48 KB 中间缓冲区 */
static esp_err_t uc8179_write_grayscale_plane(uc8179_t *controller, const uint8_t *pixels_2bpp,
                                              size_t plane_size_bytes, bool new_plane)
{
    uint8_t block[UC8179_GRAYSCALE_BLOCK_SIZE_BYTES];
    size_t  output_index = 0U;
    while (output_index < plane_size_bytes)
    {
        const size_t remaining  = plane_size_bytes - output_index;
        const size_t chunk_size = remaining < sizeof(block) ? remaining : sizeof(block);
        for (size_t index = 0U; index < chunk_size; ++index)
        {
            block[index] =
                uc8179_encode_grayscale_plane_byte(pixels_2bpp, output_index + index, new_plane);
        }

        esp_err_t error =
            controller->config.write(true, block, chunk_size, controller->config.io_context);
        if (error != ESP_OK)
        {
            return error;
        }
        output_index += chunk_size;
    }
    return ESP_OK;
}

/** @brief 把两个 2 bpp 输入字节按中间阈值编码为一个 1 bpp 黑白字节 */
static uint8_t uc8179_encode_monochrome_byte_from_grayscale(const uint8_t *pixels_2bpp,
                                                            size_t         output_index)
{
    const size_t input_index = output_index * 2U;
    return utils_gray2_pair_to_mono_byte(pixels_2bpp[input_index], pixels_2bpp[input_index + 1U]);
}

/** @brief 在线把 2 bpp 输入阈值化并写入 1 bpp 黑白帧，不分配完整中间缓冲区 */
static esp_err_t uc8179_write_monochrome_from_grayscale(uc8179_t      *controller,
                                                        const uint8_t *pixels_2bpp,
                                                        size_t         frame_size_bytes)
{
    uint8_t block[UC8179_GRAYSCALE_BLOCK_SIZE_BYTES];
    size_t  output_index = 0U;
    while (output_index < frame_size_bytes)
    {
        const size_t remaining  = frame_size_bytes - output_index;
        const size_t chunk_size = remaining < sizeof(block) ? remaining : sizeof(block);
        for (size_t index = 0U; index < chunk_size; ++index)
        {
            block[index] =
                uc8179_encode_monochrome_byte_from_grayscale(pixels_2bpp, output_index + index);
        }

        esp_err_t error =
            controller->config.write(true, block, chunk_size, controller->config.io_context);
        if (error != ESP_OK)
        {
            return error;
        }
        output_index += chunk_size;
    }
    return ESP_OK;
}

/** @brief 设置包含式 UC8179 局刷窗口，调用方保证范围和字节对齐有效 */
static esp_err_t uc8179_set_partial_window(uc8179_t *controller, const uc8179_rect_t *rect)
{
    const uint16_t x_end     = (uint16_t) (rect->x_pixels + rect->width_pixels - 1U);
    const uint16_t y_end     = (uint16_t) (rect->y_pixels + rect->height_pixels - 1U);
    const uint8_t  payload[] = {
        (uint8_t) (rect->x_pixels >> 8U),
        (uint8_t) rect->x_pixels,
        (uint8_t) (x_end >> 8U),
        (uint8_t) x_end,
        (uint8_t) (rect->y_pixels >> 8U),
        (uint8_t) rect->y_pixels,
        (uint8_t) (y_end >> 8U),
        (uint8_t) y_end,
        0x01U,
    };
    return uc8179_write_command_data(controller,
                                     UC8179_CMD_PARTIAL_WINDOW,
                                     payload,
                                     sizeof(payload));
}

/** @brief 在线转换并发送一个字节对齐矩形的 2 bpp 数据 */
static esp_err_t uc8179_write_monochrome_rect_from_grayscale(uc8179_t            *controller,
                                                             const uint8_t       *pixels_2bpp,
                                                             const uc8179_rect_t *rect)
{
    uint8_t      row[UC8179_MAX_WIDTH_PIXELS / 8U];
    const size_t source_stride_bytes = controller->config.width_pixels / 4U;
    const size_t output_width_bytes  = rect->width_pixels / 8U;
    const size_t source_x_bytes      = rect->x_pixels / 4U;

    for (uint16_t y_offset = 0U; y_offset < rect->height_pixels; ++y_offset)
    {
        const size_t source_row   = (size_t) (rect->y_pixels + y_offset) * source_stride_bytes;
        const size_t source_start = source_row + source_x_bytes;
        for (size_t x_byte = 0U; x_byte < output_width_bytes; ++x_byte)
        {
            const size_t source_index = source_start + (x_byte * 2U);
            row[x_byte] = utils_gray2_pair_to_mono_byte(pixels_2bpp[source_index],
                                                        pixels_2bpp[source_index + 1U]);
        }
        esp_err_t error =
            controller->config.write(true, row, output_width_bytes, controller->config.io_context);
        if (error != ESP_OK)
        {
            return error;
        }
    }
    return ESP_OK;
}

/** @brief 判断两个局刷矩形是否相交，边界相接不视为相交 */
static bool uc8179_rects_overlap(const uc8179_rect_t *first, const uc8179_rect_t *second)
{
    const uint32_t first_right   = (uint32_t) first->x_pixels + first->width_pixels;
    const uint32_t first_bottom  = (uint32_t) first->y_pixels + first->height_pixels;
    const uint32_t second_right  = (uint32_t) second->x_pixels + second->width_pixels;
    const uint32_t second_bottom = (uint32_t) second->y_pixels + second->height_pixels;
    return first->x_pixels < second_right && second->x_pixels < first_right
           && first->y_pixels < second_bottom && second->y_pixels < first_bottom;
}

/** @brief 校验局刷矩形范围、字节对齐和互不重叠约束 */
static bool uc8179_partial_rects_are_valid(const uc8179_t *controller, const uc8179_rect_t *rects,
                                           size_t rect_count)
{
    if (rects == NULL || rect_count == 0U)
    {
        return false;
    }
    for (size_t index = 0U; index < rect_count; ++index)
    {
        const uc8179_rect_t *rect = &rects[index];
        if (rect->width_pixels == 0U || rect->height_pixels == 0U || (rect->x_pixels % 8U) != 0U
            || (rect->width_pixels % 8U) != 0U || rect->x_pixels >= controller->config.width_pixels
            || rect->y_pixels >= controller->config.height_pixels
            || rect->width_pixels > controller->config.width_pixels - rect->x_pixels
            || rect->height_pixels > controller->config.height_pixels - rect->y_pixels)
        {
            return false;
        }
        for (size_t previous = 0U; previous < index; ++previous)
        {
            if (uc8179_rects_overlap(rect, &rects[previous]))
            {
                return false;
            }
        }
    }
    return true;
}

/** @brief 尝试退出局刷模式并保留先前产生的首个错误 */
static esp_err_t uc8179_finish_partial_window(uc8179_t *controller, esp_err_t operation_error)
{
    const esp_err_t exit_error = uc8179_write_command(controller, UC8179_CMD_PARTIAL_OUT);
    return operation_error != ESP_OK ? operation_error : exit_error;
}

esp_err_t uc8179_init(uc8179_t *out_controller, const uc8179_config_t *config)
{
    if (out_controller == NULL || config == NULL || config->write == NULL
        || config->set_reset == NULL || config->read_busy == NULL || config->delay == NULL
        || config->width_pixels == 0U || config->width_pixels > UC8179_MAX_WIDTH_PIXELS
        || (config->width_pixels % 8U) != 0U || config->height_pixels == 0U
        || config->height_pixels > UC8179_MAX_HEIGHT_PIXELS || config->busy_timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_controller->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    out_controller->config      = *config;
    out_controller->initialized = true;
    out_controller->powered_on  = false;
    return ESP_OK;
}

esp_err_t uc8179_power_on(uc8179_t *controller, uc8179_mode_t mode)
{
    if (controller == NULL || !controller->initialized || controller->powered_on)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (mode < UC8179_MODE_MONOCHROME || mode > UC8179_MODE_GRAYSCALE_4)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *booster_soft_start = mode == UC8179_MODE_GRAYSCALE_4
                                            ? controller->config.grayscale_booster_soft_start
                                            : controller->config.booster_soft_start;
    esp_err_t      error = controller->config.set_reset(false, controller->config.io_context);
    if (error == ESP_OK)
    {
        controller->config.delay(UC8179_RESET_LOW_DELAY_MS, controller->config.io_context);
        error = controller->config.set_reset(true, controller->config.io_context);
    }
    if (error == ESP_OK)
    {
        controller->config.delay(UC8179_RESET_HIGH_DELAY_MS, controller->config.io_context);
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_POWER_SETTING,
                                          controller->config.power_setting,
                                          sizeof(controller->config.power_setting));
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_BOOSTER_SOFT_START,
                                          booster_soft_start,
                                          sizeof(controller->config.booster_soft_start));
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_POWER_ON);
    }
    if (error != ESP_OK)
    {
        return error;
    }

    controller->mode       = mode;
    controller->powered_on = true;
    error                  = uc8179_wait_idle(controller, UC8179_POWER_ON_SETTLE_DELAY_MS);
    if (error != ESP_OK)
    {
        return error;
    }

    error = uc8179_write_command_data(controller,
                                      UC8179_CMD_PANEL_SETTING,
                                      &controller->config.panel_setting,
                                      sizeof(controller->config.panel_setting));

    if (error == ESP_OK)
    {
        error = uc8179_write_resolution(controller);
    }

    const uint8_t single_spi_mode = 0x00U;
    if (error == ESP_OK && mode == UC8179_MODE_MONOCHROME)
    {
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_DUAL_SPI,
                                          &single_spi_mode,
                                          sizeof(single_spi_mode));
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_VCOM_DATA_INTERVAL,
                                          controller->config.vcom_data_interval,
                                          sizeof(controller->config.vcom_data_interval));
    }
    if (error == ESP_OK && mode == UC8179_MODE_GRAYSCALE_4)
    {
        const uint8_t cascade_setting = UC8179_GRAYSCALE_CASCADE_SETTING;
        error                         = uc8179_write_command_data(controller,
                                                                  UC8179_CMD_CASCADE_SETTING,
                                                                  &cascade_setting,
                                                                  sizeof(cascade_setting));
    }
    if (error == ESP_OK && mode == UC8179_MODE_GRAYSCALE_4)
    {
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_FORCED_TEMPERATURE,
                                          &controller->config.grayscale_temperature,
                                          sizeof(controller->config.grayscale_temperature));
    }
    if (error == ESP_OK && mode == UC8179_MODE_MONOCHROME)
    {
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_TCON_SETTING,
                                          &controller->config.tcon_setting,
                                          sizeof(controller->config.tcon_setting));
    }
    return error;
}

esp_err_t uc8179_fill(uc8179_t *controller, bool black)
{
    if (controller == NULL || !controller->initialized || !controller->powered_on
        || controller->mode != UC8179_MODE_MONOCHROME)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t frame_size_bytes =
        ((size_t) controller->config.width_pixels * controller->config.height_pixels) / 8U;
    esp_err_t error = uc8179_write_command(controller, UC8179_CMD_DATA_START_OLD);
    if (error == ESP_OK)
    {
        error = uc8179_write_repeated_data(controller, 0x00U, frame_size_bytes);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_DATA_START_NEW);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_repeated_data(controller, black ? 0xFFU : 0x00U, frame_size_bytes);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_DISPLAY_REFRESH);
    }
    if (error == ESP_OK)
    {
        error = uc8179_wait_idle(controller, UC8179_REFRESH_COMMAND_DELAY_MS);
    }
    return error;
}

esp_err_t uc8179_display_monochrome_borrow(uc8179_t *controller, const uint8_t *pixels_1bpp,
                                           size_t size_bytes)
{
    if (controller == NULL || !controller->initialized || !controller->powered_on
        || controller->mode != UC8179_MODE_MONOCHROME)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t expected_size =
        ((size_t) controller->config.width_pixels * controller->config.height_pixels) / 8U;
    if (pixels_1bpp == NULL || size_bytes != expected_size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = uc8179_write_command(controller, UC8179_CMD_DATA_START_OLD);
    if (error == ESP_OK)
    {
        error = uc8179_write_repeated_data(controller, 0x00U, expected_size);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_DATA_START_NEW);
    }
    if (error == ESP_OK)
    {
        error =
            controller->config.write(true, pixels_1bpp, size_bytes, controller->config.io_context);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_DISPLAY_REFRESH);
    }
    if (error == ESP_OK)
    {
        error = uc8179_wait_idle(controller, UC8179_REFRESH_COMMAND_DELAY_MS);
    }
    return error;
}

esp_err_t uc8179_display_grayscale_borrow(uc8179_t *controller, const uint8_t *pixels_2bpp,
                                          size_t size_bytes)
{
    if (controller == NULL || !controller->initialized || !controller->powered_on
        || (controller->mode != UC8179_MODE_MONOCHROME
            && controller->mode != UC8179_MODE_GRAYSCALE_4))
    {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t expected_size =
        ((size_t) controller->config.width_pixels * controller->config.height_pixels) / 4U;
    if (pixels_2bpp == NULL || size_bytes != expected_size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t plane_size_bytes = expected_size / 2U;
    esp_err_t    error            = uc8179_write_command(controller, UC8179_CMD_DATA_START_OLD);
    if (error == ESP_OK)
    {
        error =
            controller->mode == UC8179_MODE_MONOCHROME
                ? uc8179_write_repeated_data(controller, 0x00U, plane_size_bytes)
                : uc8179_write_grayscale_plane(controller, pixels_2bpp, plane_size_bytes, false);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_DATA_START_NEW);
    }
    if (error == ESP_OK)
    {
        error =
            controller->mode == UC8179_MODE_MONOCHROME
                ? uc8179_write_monochrome_from_grayscale(controller, pixels_2bpp, plane_size_bytes)
                : uc8179_write_grayscale_plane(controller, pixels_2bpp, plane_size_bytes, true);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_DISPLAY_REFRESH);
    }
    if (error == ESP_OK)
    {
        error = uc8179_wait_idle(controller, UC8179_REFRESH_COMMAND_DELAY_MS);
    }
    return error;
}

esp_err_t uc8179_display_partial_from_gray2_borrow(uc8179_t      *controller,
                                                   const uint8_t *previous_pixels_2bpp,
                                                   const uint8_t *current_pixels_2bpp,
                                                   size_t size_bytes, const uc8179_rect_t *rects,
                                                   size_t rect_count)
{
    if (controller == NULL || !controller->initialized || !controller->powered_on
        || controller->mode != UC8179_MODE_MONOCHROME)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t expected_size =
        ((size_t) controller->config.width_pixels * controller->config.height_pixels) / 4U;
    if (previous_pixels_2bpp == NULL || current_pixels_2bpp == NULL || size_bytes != expected_size
        || !uc8179_partial_rects_are_valid(controller, rects, rect_count))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t cascade_setting = UC8179_GRAYSCALE_CASCADE_SETTING;
    esp_err_t     error           = uc8179_write_command_data(controller,
                                                              UC8179_CMD_CASCADE_SETTING,
                                                              &cascade_setting,
                                                              sizeof(cascade_setting));
    if (error == ESP_OK)
    {
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_FORCED_TEMPERATURE,
                                          &controller->config.partial_temperature,
                                          sizeof(controller->config.partial_temperature));
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_VCOM_DATA_INTERVAL,
                                          controller->config.partial_vcom_data_interval,
                                          sizeof(controller->config.partial_vcom_data_interval));
    }

    const uc8179_rect_t full_screen = {
        .x_pixels      = 0U,
        .y_pixels      = 0U,
        .width_pixels  = controller->config.width_pixels,
        .height_pixels = controller->config.height_pixels,
    };
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_PARTIAL_IN);
    }
    if (error == ESP_OK)
    {
        error = uc8179_set_partial_window(controller, &full_screen);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_DATA_START_OLD);
    }
    if (error == ESP_OK)
    {
        error = uc8179_write_monochrome_from_grayscale(controller,
                                                       previous_pixels_2bpp,
                                                       expected_size / 2U);
    }
    error = uc8179_finish_partial_window(controller, error);
    if (error != ESP_OK)
    {
        return error;
    }

    for (size_t index = 0U; index < rect_count; ++index)
    {
        error = uc8179_write_command_data(controller,
                                          UC8179_CMD_VCOM_DATA_INTERVAL,
                                          controller->config.partial_vcom_data_interval,
                                          sizeof(controller->config.partial_vcom_data_interval));
        if (error == ESP_OK)
        {
            error = uc8179_write_command(controller, UC8179_CMD_PARTIAL_IN);
        }
        if (error == ESP_OK)
        {
            error = uc8179_set_partial_window(controller, &rects[index]);
        }
        if (error == ESP_OK)
        {
            error = uc8179_write_command(controller, UC8179_CMD_DATA_START_NEW);
        }
        if (error == ESP_OK)
        {
            error = uc8179_write_monochrome_rect_from_grayscale(controller,
                                                                current_pixels_2bpp,
                                                                &rects[index]);
        }
        if (error == ESP_OK)
        {
            error = uc8179_write_command(controller, UC8179_CMD_DISPLAY_REFRESH);
        }
        if (error == ESP_OK)
        {
            error = uc8179_wait_idle(controller, UC8179_REFRESH_COMMAND_DELAY_MS);
        }
        error = uc8179_finish_partial_window(controller, error);
        if (error != ESP_OK)
        {
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t uc8179_deep_sleep(uc8179_t *controller)
{
    if (controller == NULL || !controller->initialized || !controller->powered_on)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t floating_vcom = 0xF7U;
    esp_err_t     error         = uc8179_write_command_data(controller,
                                                            UC8179_CMD_VCOM_DATA_INTERVAL,
                                                            &floating_vcom,
                                                            sizeof(floating_vcom));
    if (error == ESP_OK)
    {
        error = uc8179_write_command(controller, UC8179_CMD_POWER_OFF);
    }
    if (error == ESP_OK)
    {
        error = uc8179_wait_idle(controller, UC8179_POWER_OFF_SETTLE_DELAY_MS);
    }
    if (error == ESP_OK)
    {
        const uint8_t check_code = UC8179_DEEP_SLEEP_CHECK_CODE;
        error                    = uc8179_write_command_data(controller,
                                                             UC8179_CMD_DEEP_SLEEP,
                                                             &check_code,
                                                             sizeof(check_code));
    }
    if (error == ESP_OK)
    {
        controller->powered_on = false;
    }
    return error;
}

esp_err_t uc8179_deinit(uc8179_t *controller)
{
    if (controller == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!controller->initialized || controller->powered_on)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(controller, 0, sizeof(*controller));
    return ESP_OK;
}
