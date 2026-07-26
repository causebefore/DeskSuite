/**
 * @file bsp_epaper.c
 * @brief 装配 reTerminal E1001 的 GDEY075T7 墨水屏、SPI 和 UC8179 驱动
 */
#include "bsp.h"
#include "bsp_spi.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uc8179.h"

#define BSP_EPAPER_SPI_HOST                        SPI2_HOST
#define BSP_EPAPER_SPI_CLOCK_HZ                    10000000
#define BSP_EPAPER_WIDTH_PIXELS                    800U
#define BSP_EPAPER_HEIGHT_PIXELS                   480U
#define BSP_EPAPER_BUSY_TIMEOUT_MS                 15000U
/** @brief 临时把 2 bpp 四灰阶输入按中间阈值转换为黑白，并使用黑白全刷波形 */
#define BSP_EPAPER_GRAYSCALE_INPUT_MONOCHROME_TEST 0U
/** @brief 连续发送指定字节数后阻塞一拍，确保 Idle Task 能喂任务看门狗 */
#define BSP_EPAPER_SPI_YIELD_INTERVAL_BYTES        8192U

/** @brief 日志标签 */
static const char *TAG = "bsp_epaper";

/** @brief UC8179 SPI 设备句柄 */
static spi_device_handle_t s_spi_device;

/** @brief BSP 持有的 UC8179 驱动实例 */
static uc8179_t s_controller;

/** @brief 本模块是否持有共享 SPI2 总线引用 */
static bool s_spi_bus_acquired;

/** @brief 墨水屏 BSP 是否已初始化 */
static bool s_initialized;

/** @brief 墨水屏是否已进入深睡 */
static bool s_sleeping;

/** @brief 当前墨水屏全局刷新模式 */
static bsp_epaper_mode_t s_mode;

/** @brief 距上次主动让出 CPU 后已经完成的 SPI 字节数 */
static size_t s_spi_bytes_since_yield;

/**
 * @brief 按数据手册要求以单字节事务发送，保证 CS 每 8 位回到高电平
 *
 * 长帧仍保持逐字节 CS 时序，并在一次写回调内持有总线、使用轮询事务，避免每字节中断
 * 调度开销。每累计 8192 字节释放总线并阻塞一个 FreeRTOS Tick，让当前核心的 Idle Task
 * 获得运行机会；暂停发生在完整 8 位发送结束且 CS 已回到高电平的安全边界。
 */
static esp_err_t bsp_epaper_write(bool is_data, const uint8_t *data, size_t size_bytes,
                                  void *context)
{
    (void) context;
    if (data == NULL || size_bytes == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_EPD_DC_GPIO, is_data ? 1U : 0U),
                        TAG,
                        "设置墨水屏 D/C 电平失败");

    esp_err_t error = spi_device_acquire_bus(s_spi_device, portMAX_DELAY);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "独占墨水屏 SPI 总线失败: %s", esp_err_to_name(error));
        return error;
    }

    bool bus_acquired = true;
    for (size_t index = 0U; index < size_bytes; ++index)
    {
        spi_transaction_t transaction;
        memset(&transaction, 0, sizeof(transaction));
        transaction.flags      = SPI_TRANS_USE_TXDATA;
        transaction.length     = 8U;
        transaction.tx_data[0] = data[index];
        error                  = spi_device_polling_transmit(s_spi_device, &transaction);
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "墨水屏 SPI 发送失败: %s", esp_err_to_name(error));
            break;
        }

        ++s_spi_bytes_since_yield;
        if (s_spi_bytes_since_yield >= BSP_EPAPER_SPI_YIELD_INTERVAL_BYTES)
        {
            s_spi_bytes_since_yield = 0U;
            spi_device_release_bus(s_spi_device);
            bus_acquired = false;
            vTaskDelay(1U);

            if (index + 1U < size_bytes)
            {
                error = spi_device_acquire_bus(s_spi_device, portMAX_DELAY);
                if (error != ESP_OK)
                {
                    ESP_LOGE(TAG, "重新独占墨水屏 SPI 总线失败: %s", esp_err_to_name(error));
                    break;
                }
                bus_acquired = true;
            }
        }
    }

    if (bus_acquired)
    {
        spi_device_release_bus(s_spi_device);
    }
    return error;
}

/** @brief 设置 UC8179 低电平有效的硬件复位引脚 */
static esp_err_t bsp_epaper_set_reset(bool high, void *context)
{
    (void) context;
    return gpio_set_level(BOARD_EPD_RES_GPIO, high ? 1U : 0U);
}

/** @brief 把板级 BUSY 有效电平转换为驱动逻辑忙状态 */
static esp_err_t bsp_epaper_read_busy(bool *out_busy, void *context)
{
    (void) context;
    if (out_busy == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int level = gpio_get_level(BOARD_EPD_BUSY_GPIO);
    *out_busy       = BOARD_EPD_BUSY_ACTIVE_LOW ? level == 0 : level != 0;
    return ESP_OK;
}

/** @brief 使用 FreeRTOS 延时实现驱动所需的毫秒等待 */
static void bsp_epaper_delay(uint32_t delay_ms, void *context)
{
    (void) context;
    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    if (ticks == 0U)
    {
        ticks = 1U;
    }
    vTaskDelay(ticks);
}

/** @brief 释放已经取得的 SPI 与 GPIO 资源，用于初始化失败回滚 */
static esp_err_t bsp_epaper_release_hardware(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_spi_device != NULL)
    {
        esp_err_t error = spi_bus_remove_device(s_spi_device);
        if (first_error == ESP_OK && error != ESP_OK)
        {
            first_error = error;
        }
        s_spi_device = NULL;
    }
    if (s_spi_bus_acquired)
    {
        esp_err_t error = bsp_spi_release();
        if (first_error == ESP_OK && error != ESP_OK)
        {
            first_error = error;
        }
        if (error == ESP_OK)
        {
            s_spi_bus_acquired = false;
        }
    }

    const gpio_num_t control_pins[] = {
        BOARD_EPD_BUSY_GPIO,
        BOARD_EPD_RES_GPIO,
        BOARD_EPD_DC_GPIO,
        BOARD_EPD_CS_GPIO,
    };
    for (size_t index = 0U; index < sizeof(control_pins) / sizeof(control_pins[0]); ++index)
    {
        esp_err_t error = gpio_reset_pin(control_pins[index]);
        if (first_error == ESP_OK && error != ESP_OK)
        {
            first_error = error;
        }
    }
    return first_error;
}

/** @brief 唤醒 UC8179 并配置当前全局刷新模式，开始一次有界刷新会话 */
static esp_err_t bsp_epaper_start_refresh_session(uc8179_mode_t driver_mode)
{
    return uc8179_power_on(&s_controller, driver_mode);
}

/** @brief 根据当前 BSP 模式和黑白测试开关选择全刷驱动模式 */
static uc8179_mode_t bsp_epaper_global_refresh_driver_mode(void)
{
    const bool use_grayscale_waveform =
        s_mode == BSP_EPAPER_MODE_GRAYSCALE_4 && !BSP_EPAPER_GRAYSCALE_INPUT_MONOCHROME_TEST;
    return use_grayscale_waveform ? UC8179_MODE_GRAYSCALE_4 : UC8179_MODE_MONOCHROME;
}

/**
 * @brief 结束刷新会话，始终尝试关闭高压并进入深睡
 *
 * @param[in] operation_error 唤醒或刷新阶段的结果
 * @return 优先返回唤醒或刷新错误；前序成功时返回深睡结果
 */
static esp_err_t bsp_epaper_finish_refresh_session(esp_err_t operation_error)
{
    const esp_err_t sleep_error = uc8179_deep_sleep(&s_controller);
    if (sleep_error != ESP_OK && sleep_error != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "全刷结束后墨水屏进入深睡失败: %s", esp_err_to_name(sleep_error));
    }
    return operation_error != ESP_OK ? operation_error : sleep_error;
}

esp_err_t bsp_epaper_init(bsp_epaper_mode_t mode)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode < BSP_EPAPER_MODE_MONOCHROME || mode > BSP_EPAPER_MODE_GRAYSCALE_4)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << BOARD_EPD_RES_GPIO) | (1ULL << BOARD_EPD_DC_GPIO)
                        | (1ULL << BOARD_EPD_CS_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "墨水屏控制 GPIO 配置失败");
    (void) gpio_set_level(BOARD_EPD_RES_GPIO, 0U);
    (void) gpio_set_level(BOARD_EPD_DC_GPIO, 0U);
    (void) gpio_set_level(BOARD_EPD_CS_GPIO, 1U);

    const gpio_config_t busy_config = {
        .pin_bit_mask = (1ULL << BOARD_EPD_BUSY_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&busy_config);

    if (error == ESP_OK)
    {
        error              = bsp_spi_acquire();
        s_spi_bus_acquired = error == ESP_OK;
    }

    const spi_device_interface_config_t device_config = {
        .mode           = 0,
        .clock_speed_hz = BSP_EPAPER_SPI_CLOCK_HZ,
        .spics_io_num   = BOARD_EPD_CS_GPIO,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };
    if (error == ESP_OK)
    {
        error = spi_bus_add_device(BSP_EPAPER_SPI_HOST, &device_config, &s_spi_device);
    }

    const uc8179_config_t controller_config = {
        .width_pixels                 = BSP_EPAPER_WIDTH_PIXELS,
        .height_pixels                = BSP_EPAPER_HEIGHT_PIXELS,
        .busy_timeout_ms              = BSP_EPAPER_BUSY_TIMEOUT_MS,
        .power_setting                = { 0x07U, 0x07U, 0x3FU, 0x3FU },
        .booster_soft_start           = { 0x17U, 0x17U, 0x28U, 0x17U },
        .grayscale_booster_soft_start = { 0x27U, 0x27U, 0x18U, 0x17U },
        .panel_setting                = 0x1FU,
        .vcom_data_interval           = { 0x10U, 0x07U },
        .tcon_setting                 = 0x22U,
        .grayscale_temperature        = 0x5FU,
        .partial_temperature          = 0x6EU,
        .partial_vcom_data_interval   = { 0xA9U, 0x07U },
        .write                        = bsp_epaper_write,
        .set_reset                    = bsp_epaper_set_reset,
        .read_busy                    = bsp_epaper_read_busy,
        .delay                        = bsp_epaper_delay,
        .io_context                   = NULL,
    };
    if (error == ESP_OK)
    {
        error = uc8179_init(&s_controller, &controller_config);
    }
    if (error != ESP_OK)
    {
        (void) uc8179_deinit(&s_controller);
        (void) bsp_epaper_release_hardware();
        ESP_LOGE(TAG, "墨水屏初始化失败: %s", esp_err_to_name(error));
        return error;
    }

    s_initialized           = true;
    s_sleeping              = false;
    s_mode                  = mode;
    s_spi_bytes_since_yield = 0U;
    ESP_LOGI(
        TAG,
        "GDEY075T7 硬件初始化完成，分辨率 800x480，模式 %s，SPI 频率 10 MHz",
        mode == BSP_EPAPER_MODE_GRAYSCALE_4
            ? (BSP_EPAPER_GRAYSCALE_INPUT_MONOCHROME_TEST ? "2 bpp 输入转黑白全刷" : "4 灰阶全刷")
            : "单色全刷");
    return ESP_OK;
}

esp_err_t bsp_epaper_fill(bool black)
{
    if (!s_initialized || s_sleeping || s_mode != BSP_EPAPER_MODE_MONOCHROME)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = bsp_epaper_start_refresh_session(UC8179_MODE_MONOCHROME);
    if (error == ESP_OK)
    {
        error = uc8179_fill(&s_controller, black);
    }
    return bsp_epaper_finish_refresh_session(error);
}

esp_err_t bsp_epaper_display_monochrome_borrow(const uint8_t *pixels_1bpp, size_t size_bytes)
{
    if (!s_initialized || s_sleeping || s_mode != BSP_EPAPER_MODE_MONOCHROME)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = bsp_epaper_start_refresh_session(UC8179_MODE_MONOCHROME);
    if (error == ESP_OK)
    {
        error = uc8179_display_monochrome_borrow(&s_controller, pixels_1bpp, size_bytes);
    }
    return bsp_epaper_finish_refresh_session(error);
}

esp_err_t bsp_epaper_display_grayscale_borrow(const uint8_t *pixels_2bpp, size_t size_bytes)
{
    if (!s_initialized || s_sleeping || s_mode != BSP_EPAPER_MODE_GRAYSCALE_4)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = bsp_epaper_start_refresh_session(bsp_epaper_global_refresh_driver_mode());
    if (error == ESP_OK)
    {
        error = uc8179_display_grayscale_borrow(&s_controller, pixels_2bpp, size_bytes);
    }
    return bsp_epaper_finish_refresh_session(error);
}

esp_err_t bsp_epaper_display_partial_monochrome_borrow(const uint8_t *previous_pixels_2bpp,
                                                       const uint8_t *current_pixels_2bpp,
                                                       size_t         size_bytes,
                                                       const bsp_epaper_rect_t *rects,
                                                       size_t                   rect_count)
{
    if (!s_initialized || s_sleeping || s_mode != BSP_EPAPER_MODE_GRAYSCALE_4
        || !BSP_EPAPER_GRAYSCALE_INPUT_MONOCHROME_TEST)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (previous_pixels_2bpp == NULL || current_pixels_2bpp == NULL || rects == NULL
        || rect_count == 0U || rect_count > SIZE_MAX / sizeof(uc8179_rect_t))
    {
        return ESP_ERR_INVALID_ARG;
    }

    uc8179_rect_t *driver_rects = heap_caps_malloc(rect_count * sizeof(*driver_rects),
                                                   MALLOC_CAP_8BIT);
    if (driver_rects == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0U; index < rect_count; ++index)
    {
        driver_rects[index] = (uc8179_rect_t){
            .x_pixels      = rects[index].x_pixels,
            .y_pixels      = rects[index].y_pixels,
            .width_pixels  = rects[index].width_pixels,
            .height_pixels = rects[index].height_pixels,
        };
    }

    esp_err_t error = bsp_epaper_start_refresh_session(UC8179_MODE_MONOCHROME);
    if (error == ESP_OK)
    {
        error = uc8179_display_partial_from_gray2_borrow(&s_controller,
                                                         previous_pixels_2bpp,
                                                         current_pixels_2bpp,
                                                         size_bytes,
                                                         driver_rects,
                                                         rect_count);
    }
    heap_caps_free(driver_rects);
    return bsp_epaper_finish_refresh_session(error);
}

esp_err_t bsp_epaper_sleep(void)
{
    if (!s_initialized || s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = uc8179_deep_sleep(&s_controller);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "墨水屏进入深睡失败: %s", esp_err_to_name(error));
        return error;
    }
    s_sleeping = true;
    ESP_LOGI(TAG, "墨水屏刷新生命周期已结束，高压保持关闭");
    return ESP_OK;
}

esp_err_t bsp_epaper_deinit(void)
{
    if (!s_initialized || !s_sleeping)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(uc8179_deinit(&s_controller), TAG, "UC8179 驱动释放失败");
    esp_err_t error = bsp_epaper_release_hardware();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "墨水屏硬件资源释放失败: %s", esp_err_to_name(error));
        return error;
    }

    s_initialized           = false;
    s_sleeping              = false;
    s_mode                  = BSP_EPAPER_MODE_MONOCHROME;
    s_spi_bytes_since_yield = 0U;
    return ESP_OK;
}
