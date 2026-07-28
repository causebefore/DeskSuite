#include "bsp.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "task_stack_stats.h"

#define RLCD_BYTES_PER_ROW          (BOARD_RLCD_WIDTH / 8)
#define RLCD_BLOCK_ROWS             (BOARD_RLCD_HEIGHT / 4)
#define RLCD_FRAME_BYTES            (RLCD_BYTES_PER_ROW * BOARD_RLCD_HEIGHT)
#define RLCD_FRAMEBUFFER_COUNT      2
#define RLCD_STAGING_BUFFER_COUNT   RLCD_MAX_FLUSH_WINDOWS
/* 控制器每个 row 地址对应 RLCD_BLOCK_ROWS 字节；50 个 row 为一个 3750 字节 DMA 分块。 */
#define RLCD_DMA_CHUNK_ROWS         50
#define RLCD_DMA_BUFFER_BYTES       (RLCD_BLOCK_ROWS * RLCD_DMA_CHUNK_ROWS)
#define RLCD_DMA_BUFFER_COUNT       1
#define RLCD_COLUMN_ADDR_BASE       0x12
#define RLCD_BLOCKS_PER_COLUMN      3
#define RLCD_MAX_DIRTY_RECTS        2
#define RLCD_MAX_FLUSH_WINDOWS      2
#define RLCD_DISPLAY_TASK_STACK     3584
#define RLCD_DISPLAY_TASK_PRIO      5
#define RLCD_RESET_RELEASE_DELAY_MS 150U
#define RLCD_PARKED_OUTPUT_MASK                                                                \
    ((1ULL << BOARD_RLCD_PIN_DC) | (1ULL << BOARD_RLCD_PIN_CS) | (1ULL << BOARD_RLCD_PIN_SCLK) \
     | (1ULL << BOARD_RLCD_PIN_MOSI) | (1ULL << BOARD_RLCD_PIN_RST))
/* 显示行为/TE 时序/判黑阈值/极性见 Kconfig: DeskMate Display */

static const char *TAG = "bsp_display";

typedef struct
{
    uint32_t frame_count;
    uint32_t area_count;
    uint32_t dirty_bytes;
    uint32_t tx_bytes;
    uint32_t interval_min_us;
    uint32_t interval_max_us;
    uint64_t convert_us;
    uint64_t wait_prev_us;
    uint64_t copy_us;
    uint64_t cmd_us;
    uint64_t te_us;
    uint64_t queue_us;
    uint64_t dma_us;
    uint64_t interval_us;
} display_perf_stats_t;

typedef struct
{
    uint8_t col_start;
    uint8_t col_end;
    uint8_t row_start;
    uint8_t row_end;
    int     byte_x1;
    int     byte_x2;
    int     block_y1;
    int     block_y2;
    size_t  payload_len;
} rlcd_flush_window_t;

static uint32_t             s_partial_flush_count;
static uint32_t             s_partial_full_count;
static spi_device_handle_t  s_lcd_spi;
static esp_pm_lock_handle_t s_bus_timing_lock;
static SemaphoreHandle_t    s_flush_mutex;
static SemaphoreHandle_t    s_frame_ready_sem;
static SemaphoreHandle_t    s_te_sem;
static uint8_t             *s_framebuffer[RLCD_FRAMEBUFFER_COUNT];
static uint8_t             *s_staging_buffer[RLCD_STAGING_BUFFER_COUNT];
static uint8_t             *s_dma_buffer[RLCD_DMA_BUFFER_COUNT];
static TaskHandle_t         s_display_task;
static int                  s_draw_fb_index;

/* 一帧可携带多个分离窗口，display task 依次发送。staging_index 指向
 * PSRAM 中已经打包好的稳定窗口，发送时再分块复制到内部 DMA 缓冲区。 */
typedef struct
{
    rlcd_flush_window_t windows[RLCD_MAX_FLUSH_WINDOWS];
    int                 staging_index[RLCD_MAX_FLUSH_WINDOWS];
    int                 count;
    bool                valid;
} flush_batch_t;

static flush_batch_t s_pending_batch;
/* 待发送或正在发送的 PSRAM staging buffer 标记，防止 submit 覆盖窗口数据。 */
static bool         s_staging_locked[RLCD_STAGING_BUFFER_COUNT];
static bool         s_bus_timing_lock_held;
static bool         s_dma_active;
static bool         s_te_enabled;
static bool         s_te_timeout_warned;
static uint8_t      s_te_timeout_count;
static bool         s_initialized;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         s_accepting_frames;
static bool         s_io_held;
static bool         s_te_interrupt_suspended;

typedef struct
{
    int x1;
    int y1;
    int x2;
    int y2;
} dirty_rect_t;

static dirty_rect_t s_dirty_rects[RLCD_MAX_DIRTY_RECTS];
static int          s_dirty_count;
static uint32_t     s_flush_fps;
static uint32_t     s_flush_count_window;
static uint32_t     s_total_flush_count;
static int64_t      s_flush_window_start_us;
static int64_t      s_dma_start_us;
/* spi_device_queue_trans 内部存储指向 spi_transaction_t 的指针，直到
 * spi_device_get_trans_result 才通过它回读 tx_buffer 判断是否需要释放。
 * 栈变量在 lcd_queue_chunk 返回后失效，必须用持久存储。display task
 * 串行处理（queue 后必等 get_result 再发下一帧），单实例即可。 */
static spi_transaction_t    s_spi_transaction;
static int64_t              s_last_frame_done_us;
static int64_t              s_perf_window_start_us;
static display_perf_stats_t s_perf_stats;

static void IRAM_ATTR te_gpio_isr(void *arg)
{
    (void) arg;
    BaseType_t high_task_woken = pdFALSE;
    if (s_te_sem != NULL)
    {
        xSemaphoreGiveFromISR(s_te_sem, &high_task_woken);
    }
    if (high_task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t lcd_tx_sync(const uint8_t *data, size_t len)
{
    if (len == 0)
    {
        return ESP_OK;
    }

    spi_transaction_t transaction = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    /* polling 模式：命令阶段短事务避免队列/中断往返开销 */
    return spi_device_polling_transmit(s_lcd_spi, &transaction);
}

static esp_err_t lcd_cmd(uint8_t cmd)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_DC, 0), TAG, "设置 DC 命令电平失败");
    return lcd_tx_sync(&cmd, 1);
}

static esp_err_t lcd_data(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_DC, 1), TAG, "设置 DC 数据电平失败");
    return lcd_tx_sync(data, len);
}

/**
 * @brief 执行满足 ST7305 Sleep-Out 暖复位约束的硬件复位
 *
 * 控制器已处于 Sleep-Out 时，RSTB 释放后的 reset cancel 最长为 120 ms，且这段时间内
 * 不能再次发送 SLPOUT。保留 150 ms 余量，避免第二次及后续初始化出现低对比度雾化。
 */
static esp_err_t lcd_reset(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_RLCD_PIN_DC) | (1ULL << BOARD_RLCD_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "配置 LCD GPIO 失败");

    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_RST, 1), TAG, "RST 拉高失败");
    esp_rom_delay_us(50 * 1000);
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_RST, 0), TAG, "RST 拉低失败");
    esp_rom_delay_us(20 * 1000);
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_RST, 1), TAG, "RST 拉高失败");
    esp_rom_delay_us(RLCD_RESET_RELEASE_DELAY_MS * 1000U);
    return ESP_OK;
}

/**
 * @brief 在 SPI 总线释放后把 LCD 输出脚停靠到安全电平并禁止 Light-sleep 切换为高阻
 *
 * CS 保持高电平，SCLK/MOSI 保持低电平，RST 保持非复位高电平。DC 在 CS 为高时不参与
 * 命令解析，保持数据态高电平。TE 是面板输出，不在这里改为输出。
 */
static esp_err_t lcd_park_io_for_light_sleep(void)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_CS, 1), TAG, "预置 LCD CS 高电平失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_SCLK, 0), TAG, "预置 LCD SCLK 低电平失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_MOSI, 0), TAG, "预置 LCD MOSI 低电平失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_DC, 1), TAG, "预置 LCD DC 高电平失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_RST, 1), TAG, "预置 LCD RST 高电平失败");

    const gpio_config_t io_conf = {
        .pin_bit_mask = RLCD_PARKED_OUTPUT_MASK,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "配置 LCD 停靠 GPIO 失败");

    static const gpio_num_t parked_pins[] = {
        BOARD_RLCD_PIN_DC, BOARD_RLCD_PIN_CS, BOARD_RLCD_PIN_SCLK, BOARD_RLCD_PIN_MOSI, BOARD_RLCD_PIN_RST,
    };
    for (size_t index = 0; index < sizeof(parked_pins) / sizeof(parked_pins[0]); ++index)
    {
        ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis(parked_pins[index]),
                            TAG,
                            "禁止 LCD GPIO%d 切换睡眠态失败",
                            (int) parked_pins[index]);
    }

    return ESP_OK;
}

/**
 * @brief 在不释放 SPI matrix 的前提下切换 LCD 输出脚保持状态
 *
 * DMA 静止后 CS 已回到高电平、SCLK 已回到 mode 0 空闲低电平；GPIO hold 会在
 * Light-sleep 期间保持这些实际 pad 电平，并在解除后继续使用原 SPI 路由。
 *
 * @param[in] enabled true 启用保持，false 解除保持
 * @return ESP_OK 全部引脚已切换；其他值表示至少一个 GPIO 操作失败
 */
static esp_err_t lcd_set_io_hold(bool enabled)
{
    static const gpio_num_t held_pins[] = {
        BOARD_RLCD_PIN_DC, BOARD_RLCD_PIN_CS, BOARD_RLCD_PIN_SCLK, BOARD_RLCD_PIN_MOSI, BOARD_RLCD_PIN_RST,
    };
    esp_err_t result = ESP_OK;
    for (size_t index = 0; index < sizeof(held_pins) / sizeof(held_pins[0]); ++index)
    {
        const esp_err_t error = enabled ? gpio_hold_en(held_pins[index]) : gpio_hold_dis(held_pins[index]);
        if (result == ESP_OK && error != ESP_OK)
        {
            result = error;
        }
    }
    return result;
}

/** @brief 原子读取显示是否仍接受 framebuffer 写入与刷新提交 */
static bool display_accepts_frames(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool accepting = s_accepting_frames;
    taskEXIT_CRITICAL(&s_state_lock);
    return accepting;
}

/** @brief 原子切换显示帧入口 */
static void set_display_accepting_frames(bool accepting)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_accepting_frames = accepting;
    taskEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t lcd_init_spi(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = BOARD_RLCD_PIN_MOSI,
        .miso_io_num     = GPIO_NUM_NC,
        .sclk_io_num     = BOARD_RLCD_PIN_SCLK,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = RLCD_DMA_BUFFER_BYTES,
    };

    esp_err_t err = spi_bus_initialize(BOARD_RLCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_RETURN_ON_ERROR(err, TAG, "初始化 SPI 总线失败");
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = BOARD_RLCD_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = BOARD_RLCD_PIN_CS,
        .queue_size     = 2,
    };
    return spi_bus_add_device(BOARD_RLCD_SPI_HOST, &dev_cfg, &s_lcd_spi);
}

static esp_err_t lcd_init_te_gpio(void)
{
    s_te_sem = xSemaphoreCreateBinary();
    if (s_te_sem == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_RLCD_PIN_TE),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "配置 TE GPIO 失败");

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_RETURN_ON_ERROR(err, TAG, "安装 GPIO ISR 服务失败");
    }

    err = gpio_isr_handler_add(BOARD_RLCD_PIN_TE, te_gpio_isr, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_RETURN_ON_ERROR(err, TAG, "添加 TE GPIO ISR 失败");
    }

    s_te_enabled = CONFIG_DESKMATE_DISPLAY_TE_SYNC != 0;
    if (!s_te_enabled)
    {
        ESP_LOGI(TAG, "TE 同步等待默认关闭，仅保留 GPIO ISR 供后续诊断");
    }
    return ESP_OK;
}

static esp_err_t lcd_init_controller(void)
{
    const uint8_t d6[]          = { 0x17, 0x02 };
    const uint8_t d1[]          = { 0x01 };
    const uint8_t c0[]          = { 0x11, 0x04 };
    const uint8_t c1[]          = { 0x69, 0x69, 0x69, 0x69 };
    const uint8_t c2[]          = { 0x19, 0x19, 0x19, 0x19 };
    const uint8_t c4[]          = { 0x4B, 0x4B, 0x4B, 0x4B };
    const uint8_t d8[]          = { 0x80, 0xE9 };
    const uint8_t b2[]          = { 0x02 };
    const uint8_t b3[]          = { 0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45 };
    const uint8_t b4[]          = { 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45 };
    const uint8_t gate_timing[] = { 0x32, 0x03, 0x1F };
    const uint8_t b7[]          = { 0x13 };
    const uint8_t b0[]          = { 0x64 };
    const uint8_t c9[]          = { 0x00 };
    const uint8_t madctl[]      = { 0x48 };
    const uint8_t pixfmt[]      = { 0x11 };
    const uint8_t b9[]          = { 0x20 };
    const uint8_t b8[]          = { 0x29 };
    const uint8_t col_window[]  = { 0x12, 0x2A };
    const uint8_t row_window[]  = { 0x00, 0xC7 };
    const uint8_t tear[]        = { 0x00 };
    const uint8_t d0[]          = { 0xFF };

    ESP_RETURN_ON_ERROR(lcd_cmd(0xD6), TAG, "写 0xD6 失败");
    ESP_RETURN_ON_ERROR(lcd_data(d6, sizeof(d6)), TAG, "写 0xD6 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xD1), TAG, "写 0xD1 失败");
    ESP_RETURN_ON_ERROR(lcd_data(d1, sizeof(d1)), TAG, "写 0xD1 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xC0), TAG, "写 0xC0 失败");
    ESP_RETURN_ON_ERROR(lcd_data(c0, sizeof(c0)), TAG, "写 0xC0 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xC1), TAG, "写 0xC1 失败");
    ESP_RETURN_ON_ERROR(lcd_data(c1, sizeof(c1)), TAG, "写 0xC1 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xC2), TAG, "写 0xC2 失败");
    ESP_RETURN_ON_ERROR(lcd_data(c2, sizeof(c2)), TAG, "写 0xC2 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xC4), TAG, "写 0xC4 失败");
    ESP_RETURN_ON_ERROR(lcd_data(c4, sizeof(c4)), TAG, "写 0xC4 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xC5), TAG, "写 0xC5 失败");
    ESP_RETURN_ON_ERROR(lcd_data(c2, sizeof(c2)), TAG, "写 0xC5 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xD8), TAG, "写 0xD8 失败");
    ESP_RETURN_ON_ERROR(lcd_data(d8, sizeof(d8)), TAG, "写 0xD8 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xB2), TAG, "写 0xB2 失败");
    ESP_RETURN_ON_ERROR(lcd_data(b2, sizeof(b2)), TAG, "写 0xB2 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xB3), TAG, "写 0xB3 失败");
    ESP_RETURN_ON_ERROR(lcd_data(b3, sizeof(b3)), TAG, "写 0xB3 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xB4), TAG, "写 0xB4 失败");
    ESP_RETURN_ON_ERROR(lcd_data(b4, sizeof(b4)), TAG, "写 0xB4 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x62), TAG, "写 0x62 失败");
    ESP_RETURN_ON_ERROR(lcd_data(gate_timing, sizeof(gate_timing)), TAG, "写 0x62 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xB7), TAG, "写 0xB7 失败");
    ESP_RETURN_ON_ERROR(lcd_data(b7, sizeof(b7)), TAG, "写 0xB7 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xB0), TAG, "写 0xB0 失败");
    ESP_RETURN_ON_ERROR(lcd_data(b0, sizeof(b0)), TAG, "写 0xB0 数据失败");

    ESP_RETURN_ON_ERROR(lcd_cmd(0x11), TAG, "退出 sleep 失败");
    esp_rom_delay_us(200 * 1000);

    ESP_RETURN_ON_ERROR(lcd_cmd(0xC9), TAG, "写 0xC9 失败");
    ESP_RETURN_ON_ERROR(lcd_data(c9, sizeof(c9)), TAG, "写 0xC9 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x36), TAG, "写 MADCTL 失败");
    ESP_RETURN_ON_ERROR(lcd_data(madctl, sizeof(madctl)), TAG, "写 MADCTL 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x3A), TAG, "写像素格式失败");
    ESP_RETURN_ON_ERROR(lcd_data(pixfmt, sizeof(pixfmt)), TAG, "写像素格式数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xB9), TAG, "写 0xB9 失败");
    ESP_RETURN_ON_ERROR(lcd_data(b9, sizeof(b9)), TAG, "写 0xB9 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xB8), TAG, "写 0xB8 失败");
    ESP_RETURN_ON_ERROR(lcd_data(b8, sizeof(b8)), TAG, "写 0xB8 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x21), TAG, "开启反色失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x2A), TAG, "设置列窗口失败");
    ESP_RETURN_ON_ERROR(lcd_data(col_window, sizeof(col_window)), TAG, "写列窗口失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x2B), TAG, "设置行窗口失败");
    ESP_RETURN_ON_ERROR(lcd_data(row_window, sizeof(row_window)), TAG, "写行窗口失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x35), TAG, "设置 TE 失败");
    ESP_RETURN_ON_ERROR(lcd_data(tear, sizeof(tear)), TAG, "写 TE 数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0xD0), TAG, "设置自动电源失败");
    ESP_RETURN_ON_ERROR(lcd_data(d0, sizeof(d0)), TAG, "写自动电源数据失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x38), TAG, "关闭 idle 失败");
    ESP_RETURN_ON_ERROR(lcd_cmd(0x29), TAG, "开启显示失败");
    return ESP_OK;
}

static inline uint8_t *draw_framebuffer(void)
{
    return s_framebuffer[s_draw_fb_index];
}

static void dirty_reset(void)
{
    s_dirty_count = 0;
}

static void dirty_add_area(int x1, int y1, int x2, int y2)
{
    /* 与已有矩形相交则合并，减少矩形数量。不相交且未满则新增。
     * 已满则退化成单一包围盒，避免矩形数无限增长。 */
    for (int i = 0; i < s_dirty_count; ++i)
    {
        dirty_rect_t *r = &s_dirty_rects[i];
        if (x2 >= r->x1 && x1 <= r->x2 && y2 >= r->y1 && y1 <= r->y2)
        {
            if (x1 < r->x1)
            {
                r->x1 = x1;
            }
            if (y1 < r->y1)
            {
                r->y1 = y1;
            }
            if (x2 > r->x2)
            {
                r->x2 = x2;
            }
            if (y2 > r->y2)
            {
                r->y2 = y2;
            }
            return;
        }
    }

    if (s_dirty_count < RLCD_MAX_DIRTY_RECTS)
    {
        s_dirty_rects[s_dirty_count].x1 = x1;
        s_dirty_rects[s_dirty_count].y1 = y1;
        s_dirty_rects[s_dirty_count].x2 = x2;
        s_dirty_rects[s_dirty_count].y2 = y2;
        s_dirty_count++;
        return;
    }

    /* 已满：合并所有矩形为单一包围盒 */
    for (int i = 1; i < s_dirty_count; ++i)
    {
        if (s_dirty_rects[i].x1 < s_dirty_rects[0].x1)
        {
            s_dirty_rects[0].x1 = s_dirty_rects[i].x1;
        }
        if (s_dirty_rects[i].y1 < s_dirty_rects[0].y1)
        {
            s_dirty_rects[0].y1 = s_dirty_rects[i].y1;
        }
        if (s_dirty_rects[i].x2 > s_dirty_rects[0].x2)
        {
            s_dirty_rects[0].x2 = s_dirty_rects[i].x2;
        }
        if (s_dirty_rects[i].y2 > s_dirty_rects[0].y2)
        {
            s_dirty_rects[0].y2 = s_dirty_rects[i].y2;
        }
    }
    s_dirty_count = 1;
}

static void make_fullscreen_window(rlcd_flush_window_t *window)
{
    window->col_start   = RLCD_COLUMN_ADDR_BASE;
    window->col_end     = RLCD_COLUMN_ADDR_BASE + (RLCD_BLOCK_ROWS / RLCD_BLOCKS_PER_COLUMN) - 1;
    window->row_start   = 0;
    window->row_end     = (BOARD_RLCD_WIDTH / 2) - 1;
    window->byte_x1     = 0;
    window->byte_x2     = (BOARD_RLCD_WIDTH / 2) - 1;
    window->block_y1    = 0;
    window->block_y2    = RLCD_BLOCK_ROWS - 1;
    window->payload_len = RLCD_FRAME_BYTES;
}

/* 将单个 dirty 矩形转换为控制器窗口（含 CASET 翻转与 block 对齐）。
 * 返回窗口 payload 字节数；0 表示矩形越界无有效区域。 */
static size_t rect_to_window(rlcd_flush_window_t *window, const dirty_rect_t *rect)
{
    /* 控制器 CASET 方向与 framebuffer block_y 相反：
     *   block_y=0   -> 屏幕底部 -> CASET 最大值 0x2A
     *   block_y=74  -> 屏幕顶部 -> CASET 最小值 0x12
     * 只翻转 CASET 映射；payload 始终按 framebuffer 原始布局（block_y 递增）打包，
     * 与全屏 memcpy 完全一致。 */
    int byte_x1  = rect->x1 >> 1;
    int byte_x2  = rect->x2 >> 1;
    int block_y1 = (BOARD_RLCD_HEIGHT - 1 - rect->y2) >> 2;
    int block_y2 = (BOARD_RLCD_HEIGHT - 1 - rect->y1) >> 2;

    if (byte_x1 < 0)
    {
        byte_x1 = 0;
    }
    if (byte_x2 >= BOARD_RLCD_WIDTH / 2)
    {
        byte_x2 = (BOARD_RLCD_WIDTH / 2) - 1;
    }
    if (block_y1 < 0)
    {
        block_y1 = 0;
    }
    if (block_y2 >= RLCD_BLOCK_ROWS)
    {
        block_y2 = RLCD_BLOCK_ROWS - 1;
    }

    block_y1 = (block_y1 / RLCD_BLOCKS_PER_COLUMN) * RLCD_BLOCKS_PER_COLUMN;
    block_y2 = ((block_y2 + RLCD_BLOCKS_PER_COLUMN) / RLCD_BLOCKS_PER_COLUMN) * RLCD_BLOCKS_PER_COLUMN - 1;
    if (block_y2 >= RLCD_BLOCK_ROWS)
    {
        block_y2 = RLCD_BLOCK_ROWS - 1;
    }

    const size_t block_count = (size_t) (block_y2 - block_y1 + 1);
    const size_t row_count   = (size_t) (byte_x2 - byte_x1 + 1);
    const size_t payload_len = block_count * row_count;

    window->byte_x1          = byte_x1;
    window->byte_x2          = byte_x2;
    window->block_y1         = block_y1;
    window->block_y2         = block_y2;
    const int max_group      = (RLCD_BLOCK_ROWS / RLCD_BLOCKS_PER_COLUMN) - 1;
    const int group1         = block_y1 / RLCD_BLOCKS_PER_COLUMN;
    const int group2         = block_y2 / RLCD_BLOCKS_PER_COLUMN;
    window->col_start        = (uint8_t) (RLCD_COLUMN_ADDR_BASE + (max_group - group2));
    window->col_end          = (uint8_t) (RLCD_COLUMN_ADDR_BASE + (max_group - group1));
    window->row_start        = (uint8_t) byte_x1;
    window->row_end          = (uint8_t) byte_x2;
    window->payload_len      = payload_len;
    return payload_len;
}

static void pack_window_payload(uint8_t *dst, const uint8_t *src, const rlcd_flush_window_t *window)
{
    uint8_t     *out         = dst;
    const size_t block_count = (size_t) (window->block_y2 - window->block_y1 + 1);

    /* 按 framebuffer 原始布局打包（byte_x 递增 × block_y 递增），
     * 与全屏 memcpy 的数据顺序完全一致。 */
    for (int byte_x = window->byte_x1; byte_x <= window->byte_x2; ++byte_x)
    {
        const size_t offset = (size_t) byte_x * RLCD_BLOCK_ROWS + (size_t) window->block_y1;
        memcpy(out, src + offset, block_count);
        out += block_count;
    }
}

/* 将 dirty 矩形集合转换为最多 RLCD_MAX_FLUSH_WINDOWS 个窗口。
 * 总 payload 超阈值或不可用时退化全屏。返回窗口数量。 */
static int make_dirty_windows(rlcd_flush_window_t *windows, int max_windows)
{
    if (!CONFIG_DESKMATE_DISPLAY_PARTIAL_FLUSH || s_dirty_count == 0)
    {
        make_fullscreen_window(&windows[0]);
        return 1;
    }

    int       count       = 0;
    size_t    total_bytes = 0;
    const int limit       = s_dirty_count < max_windows ? s_dirty_count : max_windows;

    for (int i = 0; i < limit; ++i)
    {
        const size_t plen = rect_to_window(&windows[count], &s_dirty_rects[i]);
        if (plen > 0)
        {
            total_bytes += plen;
            count++;
        }
    }

    if (count == 0 || total_bytes * 100 >= RLCD_FRAME_BYTES * CONFIG_DESKMATE_DISPLAY_PARTIAL_FULL_PERCENT)
    {
        make_fullscreen_window(&windows[0]);
        return 1;
    }
    return count;
}

/* 将 dirty 矩形对应的 framebuffer 区域从 src 复制到 dst，保持双缓冲一致。 */
static size_t copy_dirty_region(uint8_t *dst, const uint8_t *src, const dirty_rect_t *rect)
{
    int byte_x1  = rect->x1 >> 1;
    int byte_x2  = rect->x2 >> 1;
    int block_y1 = (BOARD_RLCD_HEIGHT - 1 - rect->y2) >> 2;
    int block_y2 = (BOARD_RLCD_HEIGHT - 1 - rect->y1) >> 2;
    if (byte_x1 < 0)
    {
        byte_x1 = 0;
    }
    if (byte_x2 >= BOARD_RLCD_WIDTH / 2)
    {
        byte_x2 = (BOARD_RLCD_WIDTH / 2) - 1;
    }
    if (block_y1 < 0)
    {
        block_y1 = 0;
    }
    if (block_y2 >= RLCD_BLOCK_ROWS)
    {
        block_y2 = RLCD_BLOCK_ROWS - 1;
    }
    block_y1 = (block_y1 / RLCD_BLOCKS_PER_COLUMN) * RLCD_BLOCKS_PER_COLUMN;
    block_y2 = ((block_y2 + RLCD_BLOCKS_PER_COLUMN) / RLCD_BLOCKS_PER_COLUMN) * RLCD_BLOCKS_PER_COLUMN - 1;
    if (block_y2 >= RLCD_BLOCK_ROWS)
    {
        block_y2 = RLCD_BLOCK_ROWS - 1;
    }

    const size_t block_count = (size_t) (block_y2 - block_y1 + 1);
    size_t       copied      = 0;
    for (int byte_x = byte_x1; byte_x <= byte_x2; ++byte_x)
    {
        const size_t offset = (size_t) byte_x * RLCD_BLOCK_ROWS + (size_t) block_y1;
        memcpy(dst + offset, src + offset, block_count);
        copied += block_count;
    }
    return copied;
}

static inline bool rgb565_is_black_fast(uint16_t color)
{
    if (color == 0x0000)
    {
        return true;
    }
    if (color == 0xffff)
    {
        return false;
    }

    const uint32_t r         = (uint32_t) ((color >> 11) & 0x1f);
    const uint32_t g         = (uint32_t) ((color >> 5) & 0x3f);
    const uint32_t b         = (uint32_t) (color & 0x1f);
    const uint32_t luminance = r * 77U + g * 150U + b * 29U;
    return luminance < (uint32_t) CONFIG_DESKMATE_DISPLAY_RGB565_BLACK_THRESHOLD;
}

static inline void write_rgb565_pixel(uint8_t *fb, int x, uint8_t row_bit_base, uint16_t block_y, uint16_t color)
{
    const size_t  offset = (size_t) (x >> 1) * RLCD_BLOCK_ROWS + block_y;
    const uint8_t mask   = (uint8_t) (1U << (7U - (row_bit_base + (uint8_t) (x & 0x01))));

    if (rgb565_is_black_fast(color))
    {
        fb[offset] &= (uint8_t) ~mask;
    }
    else
    {
        fb[offset] |= mask;
    }
}

static inline void write_rgb565_pair(uint8_t *fb, int even_x, uint16_t block_y, uint8_t left_mask, uint8_t right_mask,
                                     uint8_t pair_mask, const uint16_t *pixels)
{
    const size_t offset = (size_t) (even_x >> 1) * RLCD_BLOCK_ROWS + block_y;
    uint8_t      value  = 0;

    if (!rgb565_is_black_fast(pixels[0]))
    {
        value |= left_mask;
    }
    if (!rgb565_is_black_fast(pixels[1]))
    {
        value |= right_mask;
    }

    fb[offset] = (uint8_t) ((fb[offset] & (uint8_t) ~pair_mask) | value);
}

static void write_rgb565_row(uint8_t *fb, int x1, int x2, int y, const uint16_t *pixels)
{
    const uint16_t inv_y        = (uint16_t) (BOARD_RLCD_HEIGHT - 1 - y);
    const uint16_t block_y      = (uint16_t) (inv_y >> 2);
    const uint8_t  row_bit_base = (uint8_t) ((inv_y & 0x03U) << 1);
    const uint8_t  left_mask    = (uint8_t) (1U << (7U - row_bit_base));
    const uint8_t  right_mask   = (uint8_t) (1U << (6U - row_bit_base));
    const uint8_t  pair_mask    = (uint8_t) (left_mask | right_mask);

    int x                       = x1;
    if ((x & 0x01) != 0 && x <= x2)
    {
        write_rgb565_pixel(fb, x, row_bit_base, block_y, *pixels++);
        ++x;
    }

    for (; x + 1 <= x2; x += 2, pixels += 2)
    {
        write_rgb565_pair(fb, x, block_y, left_mask, right_mask, pair_mask, pixels);
    }

    if (x <= x2)
    {
        write_rgb565_pixel(fb, x, row_bit_base, block_y, *pixels);
    }
}

static void write_rgb565_fullscreen(uint8_t *fb, const uint16_t *pixels)
{
    for (int y = 0; y < BOARD_RLCD_HEIGHT; ++y)
    {
        const uint16_t  inv_y        = (uint16_t) (BOARD_RLCD_HEIGHT - 1 - y);
        const uint16_t  block_y      = (uint16_t) (inv_y >> 2);
        const uint8_t   row_bit_base = (uint8_t) ((inv_y & 0x03U) << 1);
        const uint8_t   left_mask    = (uint8_t) (1U << (7U - row_bit_base));
        const uint8_t   right_mask   = (uint8_t) (1U << (6U - row_bit_base));
        const uint8_t   pair_mask    = (uint8_t) (left_mask | right_mask);
        const uint16_t *row          = pixels + (size_t) y * BOARD_RLCD_WIDTH;

        for (int x = 0; x < BOARD_RLCD_WIDTH; x += 2)
        {
            write_rgb565_pair(fb, x, block_y, left_mask, right_mask, pair_mask, row + x);
        }
    }
}

/* ---- I1（1-bit）直出路径 ----
 * LVGL 以 LV_COLOR_FORMAT_I1 渲染，px_map 为标准行优先 bit 打包
 *（每 byte 8 像素，MSB=最左，stride 由调用方传入，随 area 宽度变化）。
 * 控制器 framebuffer 是"2 像素列 × 4 行"列块打包 + Y 翻转，因此需要逐像素
 * bit 转换。相比 RGB565 路径省掉了逐像素亮度加权计算。 */

static inline bool i1_read_pixel(const uint8_t *src_row, int bit_offset, int j)
{
    const uint8_t byte = src_row[(bit_offset + j) >> 3];
    const uint8_t bit  = (uint8_t) (7U - (uint8_t) ((bit_offset + j) & 7U));
    const bool    on   = (byte >> bit) & 1U;
#if CONFIG_DESKMATE_DISPLAY_I1_INVERT
    return !on;
#else
    return on;
#endif
}

static void write_i1_row(uint8_t *fb, int x1, int x2, int y, const uint8_t *src_row, int bit_offset)
{
    const uint16_t inv_y        = (uint16_t) (BOARD_RLCD_HEIGHT - 1 - y);
    const uint16_t block_y      = (uint16_t) (inv_y >> 2);
    const uint8_t  row_bit_base = (uint8_t) ((inv_y & 0x03U) << 1);

    int x                       = x1;
    int j                       = 0;

    /* 处理奇数起始像素，使后续可成对处理 */
    if ((x & 0x01) != 0 && x <= x2)
    {
        const size_t  offset = (size_t) (x >> 1) * RLCD_BLOCK_ROWS + block_y;
        const uint8_t mask   = (uint8_t) (1U << (7U - (row_bit_base + 1U)));
        if (i1_read_pixel(src_row, bit_offset, j))
        {
            fb[offset] |= mask;
        }
        else
        {
            fb[offset] &= (uint8_t) ~mask;
        }
        ++x;
        ++j;
    }

    for (; x + 1 <= x2; x += 2, j += 2)
    {
        const size_t  offset    = (size_t) (x >> 1) * RLCD_BLOCK_ROWS + block_y;
        const uint8_t left_bit  = (uint8_t) (7U - row_bit_base);
        const uint8_t right_bit = (uint8_t) (6U - row_bit_base);
        uint8_t       value     = 0;
        if (i1_read_pixel(src_row, bit_offset, j))
        {
            value |= (uint8_t) (1U << left_bit);
        }
        if (i1_read_pixel(src_row, bit_offset, j + 1))
        {
            value |= (uint8_t) (1U << right_bit);
        }
        const uint8_t pair_mask = (uint8_t) ((1U << left_bit) | (1U << right_bit));
        fb[offset]              = (uint8_t) ((fb[offset] & (uint8_t) ~pair_mask) | value);
    }

    if (x <= x2)
    {
        const size_t  offset = (size_t) (x >> 1) * RLCD_BLOCK_ROWS + block_y;
        const uint8_t mask   = (uint8_t) (1U << (7U - row_bit_base));
        if (i1_read_pixel(src_row, bit_offset, j))
        {
            fb[offset] |= mask;
        }
        else
        {
            fb[offset] &= (uint8_t) ~mask;
        }
    }
}

static void write_i1_fullscreen(uint8_t *fb, const uint8_t *px_map, uint32_t stride)
{
    for (int y = 0; y < BOARD_RLCD_HEIGHT; ++y)
    {
        const uint8_t *src_row = px_map + (size_t) y * stride;
        write_i1_row(fb, 0, BOARD_RLCD_WIDTH - 1, y, src_row, 0);
    }
}

static TickType_t timeout_to_ticks(uint32_t timeout_ms)
{
    return timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

static void record_flush_done(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_flush_window_start_us == 0)
    {
        s_flush_window_start_us = now_us;
    }

    s_flush_count_window++;
    s_total_flush_count++;
    const int64_t elapsed_us = now_us - s_flush_window_start_us;
    if (elapsed_us >= 1000 * 1000)
    {
        s_flush_fps             = (uint32_t) ((s_flush_count_window * 1000 * 1000) / elapsed_us);
        s_flush_count_window    = 0;
        s_flush_window_start_us = now_us;
    }

    if (s_dma_start_us > 0)
    {
        s_perf_stats.dma_us += (uint64_t) (now_us - s_dma_start_us);
        s_dma_start_us = 0;
    }

    if (s_last_frame_done_us > 0)
    {
        const uint32_t interval_us = (uint32_t) (now_us - s_last_frame_done_us);
        s_perf_stats.interval_us += interval_us;
        if (s_perf_stats.interval_min_us == 0 || interval_us < s_perf_stats.interval_min_us)
        {
            s_perf_stats.interval_min_us = interval_us;
        }
        if (interval_us > s_perf_stats.interval_max_us)
        {
            s_perf_stats.interval_max_us = interval_us;
        }
    }
    s_last_frame_done_us = now_us;
}

static void log_perf_stats_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_perf_window_start_us == 0)
    {
        s_perf_window_start_us = now_us;
        return;
    }

    const int64_t elapsed_us = now_us - s_perf_window_start_us;
    if (elapsed_us < CONFIG_DESKMATE_DISPLAY_PERF_LOG_INTERVAL_US || s_perf_stats.frame_count == 0)
    {
        return;
    }

    const uint32_t frames          = s_perf_stats.frame_count;
    const uint32_t areas           = s_perf_stats.area_count;
    const uint32_t interval_avg_us = s_perf_stats.interval_us > 0 ? (uint32_t) (s_perf_stats.interval_us / frames) : 0;
    ESP_LOGI(
        TAG,
        "perf fps=%lu frames=%lu areas=%lu interval_us avg=%lu min=%lu max=%lu avg_us: convert=%llu wait_prev=%llu copy=%llu cmd=%llu te=%llu queue=%llu dma=%llu dirty_bytes=%lu tx_bytes=%lu partial=%lu full=%lu",
        (unsigned long) ((uint64_t) frames * 1000ULL * 1000ULL / (uint64_t) elapsed_us),
        (unsigned long) frames,
        (unsigned long) areas,
        (unsigned long) interval_avg_us,
        (unsigned long) s_perf_stats.interval_min_us,
        (unsigned long) s_perf_stats.interval_max_us,
        (unsigned long long) (s_perf_stats.convert_us / frames),
        (unsigned long long) (s_perf_stats.wait_prev_us / frames),
        (unsigned long long) (s_perf_stats.copy_us / frames),
        (unsigned long long) (s_perf_stats.cmd_us / frames),
        (unsigned long long) (s_perf_stats.te_us / frames),
        (unsigned long long) (s_perf_stats.queue_us / frames),
        (unsigned long long) (s_perf_stats.dma_us / frames),
        (unsigned long) (s_perf_stats.dirty_bytes / frames),
        (unsigned long) (s_perf_stats.tx_bytes / frames),
        (unsigned long) s_partial_flush_count,
        (unsigned long) s_partial_full_count);

    memset(&s_perf_stats, 0, sizeof(s_perf_stats));
    s_partial_flush_count  = 0;
    s_partial_full_count   = 0;
    s_perf_window_start_us = now_us;
}

static void wait_te_signal(void)
{
    if (!s_te_enabled || s_te_sem == NULL)
    {
        return;
    }

    while (xSemaphoreTake(s_te_sem, 0) == pdTRUE)
    {
    }

    if (xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(CONFIG_DESKMATE_DISPLAY_TE_WAIT_TIMEOUT_MS)) == pdTRUE)
    {
        s_te_timeout_count = 0;
        return;
    }

    s_te_timeout_count++;
    if (!s_te_timeout_warned)
    {
        ESP_LOGW(TAG, "等待 TE 信号超时，先继续按异步 SPI 刷新");
        s_te_timeout_warned = true;
    }
    if (s_te_timeout_count >= CONFIG_DESKMATE_DISPLAY_TE_MAX_TIMEOUTS)
    {
        s_te_enabled = false;
        ESP_LOGW(TAG,
                 "连续 %u 次未收到 TE 信号，已关闭 TE 同步等待",
                 (unsigned) CONFIG_DESKMATE_DISPLAY_TE_MAX_TIMEOUTS);
    }
}

static esp_err_t lcd_queue_chunk(const uint8_t *frame, const rlcd_flush_window_t *window, bool wait_for_te)
{
    if (frame == NULL || window == NULL || window->payload_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t segment_start_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(lcd_cmd(0x2A), TAG, "设置列地址失败");
    const uint8_t column_addr[] = { window->col_start, window->col_end };
    ESP_RETURN_ON_ERROR(lcd_data(column_addr, sizeof(column_addr)), TAG, "写列地址失败");

    ESP_RETURN_ON_ERROR(lcd_cmd(0x2B), TAG, "设置行地址失败");
    const uint8_t row_addr[] = { window->row_start, window->row_end };
    ESP_RETURN_ON_ERROR(lcd_data(row_addr, sizeof(row_addr)), TAG, "写行地址失败");
    s_perf_stats.cmd_us += (uint64_t) (esp_timer_get_time() - segment_start_us);

    if (wait_for_te)
    {
        segment_start_us = esp_timer_get_time();
        wait_te_signal();
        s_perf_stats.te_us += (uint64_t) (esp_timer_get_time() - segment_start_us);
    }

    segment_start_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(lcd_cmd(0x2C), TAG, "写内存命令失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_RLCD_PIN_DC, 1), TAG, "设置 DC 数据电平失败");
    s_perf_stats.cmd_us += (uint64_t) (esp_timer_get_time() - segment_start_us);

    if (s_bus_timing_lock != NULL)
    {
        esp_pm_lock_acquire(s_bus_timing_lock);
        s_bus_timing_lock_held = true;
    }

    s_spi_transaction = (spi_transaction_t) {
        .length    = window->payload_len * 8,
        .tx_buffer = frame,
    };
    s_perf_stats.tx_bytes += (uint32_t) window->payload_len;
    /* queue 模式：提交后立即返回，DMA 在后台传输。pm lock 由
     * display_flush_task 在 get_trans_result 之后释放。 */
    s_dma_start_us      = esp_timer_get_time();
    const esp_err_t err = spi_device_queue_trans(s_lcd_spi, &s_spi_transaction, portMAX_DELAY);
    if (err != ESP_OK)
    {
        s_dma_start_us = 0;
        if (s_bus_timing_lock != NULL && s_bus_timing_lock_held)
        {
            esp_pm_lock_release(s_bus_timing_lock);
            s_bus_timing_lock_held = false;
        }
    }
    return err;
}

/* 等待一个 DMA 分块完成并释放 pm lock；窗口级完成统计由调用方在全部分块结束后记录。 */
static esp_err_t finish_in_flight_dma(bool *in_flight)
{
    if (!*in_flight)
    {
        return ESP_OK;
    }
    spi_transaction_t *ret_trans = NULL;
    const esp_err_t    wait_err  = spi_device_get_trans_result(s_lcd_spi, &ret_trans, portMAX_DELAY);
    if (s_bus_timing_lock != NULL && s_bus_timing_lock_held)
    {
        esp_pm_lock_release(s_bus_timing_lock);
        s_bus_timing_lock_held = false;
    }
    if (s_dma_start_us > 0)
    {
        s_perf_stats.dma_us += (uint64_t) (esp_timer_get_time() - s_dma_start_us);
        s_dma_start_us = 0;
    }
    *in_flight = false;
    return wait_err;
}

/**
 * @brief 把一个已打包窗口拆成有界 DMA 分块并串行发送
 *
 * staging 数据按 byte_x 递增排列；每个分块只缩小控制器行窗口，列窗口保持不变。
 * 首个分块等待一次 TE，后续分块连续发送，完整窗口结束后才更新刷新完成统计。
 */
static esp_err_t send_staged_window(const uint8_t *staging, const rlcd_flush_window_t *window)
{
    if (staging == NULL || window == NULL || window->payload_len == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (window->payload_len < RLCD_FRAME_BYTES)
    {
        s_partial_flush_count++;
    }
    else
    {
        s_partial_full_count++;
    }

    const size_t block_count = (size_t) (window->block_y2 - window->block_y1 + 1);
    if (block_count == 0U)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t rows_per_chunk = RLCD_DMA_BUFFER_BYTES / block_count;
    if (rows_per_chunk == 0U)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    bool   in_flight      = false;
    size_t staging_offset = 0U;
    bool   first_chunk    = true;
    for (int byte_x = window->byte_x1; byte_x <= window->byte_x2;)
    {
        ESP_RETURN_ON_ERROR(finish_in_flight_dma(&in_flight), TAG, "等待上一显示 DMA 分块失败");

        const size_t remaining_rows = (size_t) (window->byte_x2 - byte_x + 1);
        const size_t chunk_rows     = remaining_rows < rows_per_chunk ? remaining_rows : rows_per_chunk;
        const size_t chunk_bytes    = chunk_rows * block_count;
        memcpy(s_dma_buffer[0], staging + staging_offset, chunk_bytes);

        rlcd_flush_window_t chunk = *window;
        chunk.byte_x1             = byte_x;
        chunk.byte_x2             = byte_x + (int) chunk_rows - 1;
        chunk.row_start           = (uint8_t) chunk.byte_x1;
        chunk.row_end             = (uint8_t) chunk.byte_x2;
        chunk.payload_len         = chunk_bytes;

        const esp_err_t error     = lcd_queue_chunk(s_dma_buffer[0], &chunk, first_chunk);
        if (error != ESP_OK)
        {
            return error;
        }
        in_flight   = true;
        first_chunk = false;
        staging_offset += chunk_bytes;
        byte_x = chunk.byte_x2 + 1;
    }

    ESP_RETURN_ON_ERROR(finish_in_flight_dma(&in_flight), TAG, "等待显示 DMA 尾分块失败");
    record_flush_done();
    return ESP_OK;
}

static void display_flush_task(void *arg)
{
    (void) arg;

    task_stack_stats_t stack_stats = TASK_STACK_STATS_INITIALIZER;
    while (true)
    {
        task_stack_stats_log_if_due(&stack_stats, "rlcd_flush");
        xSemaphoreTake(s_frame_ready_sem, portMAX_DELAY);

        while (true)
        {
            /* 取出一帧（可能含多个分离窗口）；staging 在 submit 时已经锁定。 */
            flush_batch_t batch;
            bool          has_batch = false;
            if (xSemaphoreTake(s_flush_mutex, portMAX_DELAY) == pdTRUE)
            {
                if (s_pending_batch.valid)
                {
                    batch                 = s_pending_batch;
                    s_pending_batch.valid = false;
                    has_batch             = true;
                    s_dma_active          = true;
                }
                xSemaphoreGive(s_flush_mutex);
            }
            if (!has_batch)
            {
                break;
            }

            for (int w = 0; w < batch.count; ++w)
            {
                const int       staging_index = batch.staging_index[w];
                const esp_err_t err           = send_staged_window(s_staging_buffer[staging_index], &batch.windows[w]);
                if (xSemaphoreTake(s_flush_mutex, portMAX_DELAY) == pdTRUE)
                {
                    s_staging_locked[staging_index] = false;
                    xSemaphoreGive(s_flush_mutex);
                }
                if (err == ESP_OK)
                {
                    s_perf_stats.frame_count++;
                    log_perf_stats_if_due();
                }
                else
                {
                    ESP_LOGE(TAG, "提交 RLCD DMA 失败: %s", esp_err_to_name(err));
                    if (xSemaphoreTake(s_flush_mutex, portMAX_DELAY) == pdTRUE)
                    {
                        for (int pending = w + 1; pending < batch.count; ++pending)
                        {
                            s_staging_locked[batch.staging_index[pending]] = false;
                        }
                        xSemaphoreGive(s_flush_mutex);
                    }
                    break;
                }
            }
        }

        if (xSemaphoreTake(s_flush_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_dma_active = s_pending_batch.valid;
            xSemaphoreGive(s_flush_mutex);
        }
    }
}

static esp_err_t submit_flush_batch(const uint8_t *frame, rlcd_flush_window_t *windows, int count)
{
    if (s_frame_ready_sem == NULL || frame == NULL || windows == NULL || count <= 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t segment_start_us = esp_timer_get_time();
    if (xSemaphoreTake(s_flush_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    if (!display_accepts_frames())
    {
        xSemaphoreGive(s_flush_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Binary 通知只保留最新一帧；若 display task 尚未取走旧 pending，
     * 先释放旧 staging，再用本次完整窗口替换，避免 PSRAM staging 永久锁定。 */
    if (s_pending_batch.valid)
    {
        for (int i = 0; i < s_pending_batch.count; ++i)
        {
            s_staging_locked[s_pending_batch.staging_index[i]] = false;
        }
        s_pending_batch.valid = false;
    }

    /* 统计可用 staging buffer：待发送或正在发送的窗口不能被覆盖。 */
    int avail_count = 0;
    int avail_staging[RLCD_STAGING_BUFFER_COUNT];
    for (int i = 0; i < RLCD_STAGING_BUFFER_COUNT; ++i)
    {
        if (!s_staging_locked[i])
        {
            avail_staging[avail_count++] = i;
        }
    }

    /* buffer 不足以容纳全部窗口：剩余窗口合并进最后一个（取包围盒）。 */
    if (count > avail_count && avail_count > 0)
    {
        rlcd_flush_window_t merged = windows[avail_count - 1];
        for (int i = avail_count; i < count; ++i)
        {
            if (windows[i].byte_x1 < merged.byte_x1)
            {
                merged.byte_x1 = windows[i].byte_x1;
            }
            if (windows[i].byte_x2 > merged.byte_x2)
            {
                merged.byte_x2 = windows[i].byte_x2;
            }
            if (windows[i].block_y1 < merged.block_y1)
            {
                merged.block_y1 = windows[i].block_y1;
            }
            if (windows[i].block_y2 > merged.block_y2)
            {
                merged.block_y2 = windows[i].block_y2;
            }
            const size_t bc     = (size_t) (merged.block_y2 - merged.block_y1 + 1);
            const size_t rc     = (size_t) (merged.byte_x2 - merged.byte_x1 + 1);
            merged.payload_len  = bc * rc;
            const int max_group = (RLCD_BLOCK_ROWS / RLCD_BLOCKS_PER_COLUMN) - 1;
            const int group1    = merged.block_y1 / RLCD_BLOCKS_PER_COLUMN;
            const int group2    = merged.block_y2 / RLCD_BLOCKS_PER_COLUMN;
            merged.col_start    = (uint8_t) (RLCD_COLUMN_ADDR_BASE + (max_group - group2));
            merged.col_end      = (uint8_t) (RLCD_COLUMN_ADDR_BASE + (max_group - group1));
            merged.row_start    = (uint8_t) merged.byte_x1;
            merged.row_end      = (uint8_t) merged.byte_x2;
        }
        windows[avail_count - 1] = merged;
        count                    = avail_count;
    }

    if (avail_count == 0)
    {
        xSemaphoreGive(s_flush_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < count; ++i)
    {
        const int staging_index = avail_staging[i];
        pack_window_payload(s_staging_buffer[staging_index], frame, &windows[i]);
        s_staging_locked[staging_index]  = true;
        s_pending_batch.windows[i]       = windows[i];
        s_pending_batch.staging_index[i] = staging_index;
    }
    s_pending_batch.count = count;
    s_pending_batch.valid = true;
    xSemaphoreGive(s_flush_mutex);

    xSemaphoreGive(s_frame_ready_sem);
    s_perf_stats.queue_us += (uint64_t) (esp_timer_get_time() - segment_start_us);
    return ESP_OK;
}

esp_err_t bsp_display_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_flush_mutex = xSemaphoreCreateMutex();
    if (s_flush_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_frame_ready_sem = xSemaphoreCreateBinary();
    if (s_frame_ready_sem == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < RLCD_FRAMEBUFFER_COUNT; ++i)
    {
        s_framebuffer[i] =
            /* framebuffer 是 CPU 工作面（写入 + pack 拷出），无 DMA 需求，放 PSRAM 腾出内部 RAM。
             * DMA buffer 仍保持内部 RAM。 */
            heap_caps_malloc(RLCD_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_framebuffer[i] == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        memset(s_framebuffer[i], 0xff, RLCD_FRAME_BYTES);
    }

    for (int i = 0; i < RLCD_STAGING_BUFFER_COUNT; ++i)
    {
        s_staging_buffer[i] = heap_caps_malloc(RLCD_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_staging_buffer[i] == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
        s_staging_locked[i] = false;
    }

    for (int i = 0; i < RLCD_DMA_BUFFER_COUNT; ++i)
    {
        s_dma_buffer[i] = heap_caps_malloc(RLCD_DMA_BUFFER_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_dma_buffer[i] == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t lock_err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "rlcd_spi", &s_bus_timing_lock);
    if (lock_err != ESP_OK)
    {
        ESP_LOGW(TAG, "创建 SPI 总线时序锁失败: %s，SPI 仍会继续工作", esp_err_to_name(lock_err));
        s_bus_timing_lock = NULL;
    }

    dirty_reset();
    dirty_add_area(0, 0, BOARD_RLCD_WIDTH - 1, BOARD_RLCD_HEIGHT - 1);

    ESP_RETURN_ON_ERROR(lcd_init_spi(), TAG, "SPI 初始化失败");
    ESP_RETURN_ON_ERROR(lcd_init_te_gpio(), TAG, "TE GPIO 初始化失败");
    ESP_RETURN_ON_ERROR(lcd_reset(), TAG, "LCD 复位失败");
    ESP_RETURN_ON_ERROR(lcd_init_controller(), TAG, "LCD 控制器初始化失败");

    const BaseType_t task_ok = xTaskCreate(display_flush_task,
                                           "rlcd_flush",
                                           RLCD_DISPLAY_TASK_STACK,
                                           NULL,
                                           RLCD_DISPLAY_TASK_PRIO,
                                           &s_display_task);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "创建 RLCD 刷新任务失败");

    set_display_accepting_frames(true);
    ESP_RETURN_ON_ERROR(bsp_display_request_flush(), TAG, "初始清屏失败");
    ESP_RETURN_ON_ERROR(bsp_display_wait_flush_done(1000), TAG, "等待初始清屏失败");

    s_initialized = true;
    ESP_LOGI(TAG,
             "RLCD 初始化完成: %dx%d, SPI=%u Hz, framebuffer=%u bytes x2, "
             "staging=%u bytes x%u PSRAM, dma=%u bytes x%u SRAM 分块异步",
             BOARD_RLCD_WIDTH,
             BOARD_RLCD_HEIGHT,
             (unsigned) BOARD_RLCD_SPI_HZ,
             (unsigned) RLCD_FRAME_BYTES,
             (unsigned) RLCD_FRAME_BYTES,
             (unsigned) RLCD_STAGING_BUFFER_COUNT,
             (unsigned) RLCD_DMA_BUFFER_BYTES,
             (unsigned) RLCD_DMA_BUFFER_COUNT);
    return ESP_OK;
}

esp_err_t bsp_display_write_rgb565_area(int x1, int y1, int x2, int y2, const uint16_t *pixels, int stride_pixels)
{
    uint8_t *fb = draw_framebuffer();
    if (!s_initialized || !display_accepts_frames() || fb == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || stride_pixels <= 0 || x2 < x1 || y2 < y1)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (x1 == 0 && y1 == 0 && x2 == BOARD_RLCD_WIDTH - 1 && y2 == BOARD_RLCD_HEIGHT - 1
        && stride_pixels == BOARD_RLCD_WIDTH)
    {
        const int64_t convert_start_us = esp_timer_get_time();
        write_rgb565_fullscreen(fb, pixels);
        s_perf_stats.convert_us += (uint64_t) (esp_timer_get_time() - convert_start_us);
        dirty_add_area(0, 0, BOARD_RLCD_WIDTH - 1, BOARD_RLCD_HEIGHT - 1);
        s_perf_stats.area_count++;
        return ESP_OK;
    }

    const int clipped_x1 = x1 < 0 ? 0 : x1;
    const int clipped_y1 = y1 < 0 ? 0 : y1;
    const int clipped_x2 = x2 >= BOARD_RLCD_WIDTH ? (BOARD_RLCD_WIDTH - 1) : x2;
    const int clipped_y2 = y2 >= BOARD_RLCD_HEIGHT ? (BOARD_RLCD_HEIGHT - 1) : y2;
    if (clipped_x2 < clipped_x1 || clipped_y2 < clipped_y1)
    {
        return ESP_OK;
    }

    const int64_t convert_start_us = esp_timer_get_time();
    for (int y = clipped_y1; y <= clipped_y2; ++y)
    {
        const uint16_t *src_row = pixels + (size_t) (y - y1) * (size_t) stride_pixels + (size_t) (clipped_x1 - x1);
        write_rgb565_row(fb, clipped_x1, clipped_x2, y, src_row);
    }
    s_perf_stats.convert_us += (uint64_t) (esp_timer_get_time() - convert_start_us);
    dirty_add_area(clipped_x1, clipped_y1, clipped_x2, clipped_y2);
    s_perf_stats.area_count++;
    return ESP_OK;
}

esp_err_t bsp_display_write_i1_area(int x1, int y1, int x2, int y2, const uint8_t *px_map, uint32_t stride_bytes)
{
    uint8_t *fb = draw_framebuffer();
    if (!s_initialized || !display_accepts_frames() || fb == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (px_map == NULL || stride_bytes == 0 || x2 < x1 || y2 < y1)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int clipped_x1 = x1 < 0 ? 0 : x1;
    const int clipped_y1 = y1 < 0 ? 0 : y1;
    const int clipped_x2 = x2 >= BOARD_RLCD_WIDTH ? (BOARD_RLCD_WIDTH - 1) : x2;
    const int clipped_y2 = y2 >= BOARD_RLCD_HEIGHT ? (BOARD_RLCD_HEIGHT - 1) : y2;
    if (clipped_x2 < clipped_x1 || clipped_y2 < clipped_y1)
    {
        return ESP_OK;
    }

    /* PARTIAL 模式下 draw_buf 被 reshape 为 area 宽度，px_map 列 0 = area->x1，
     * 字节内无额外 bit 偏移。 */
    const int bit_offset           = 0;

    const int64_t convert_start_us = esp_timer_get_time();
    if (clipped_x1 == 0 && clipped_y1 == 0 && clipped_x2 == BOARD_RLCD_WIDTH - 1 && clipped_y2 == BOARD_RLCD_HEIGHT - 1)
    {
        write_i1_fullscreen(fb, px_map, stride_bytes);
    }
    else
    {
        for (int y = clipped_y1; y <= clipped_y2; ++y)
        {
            const uint8_t *src_row = px_map + (size_t) (y - y1) * stride_bytes;
            write_i1_row(fb, clipped_x1, clipped_x2, y, src_row, bit_offset);
        }
    }
    s_perf_stats.convert_us += (uint64_t) (esp_timer_get_time() - convert_start_us);
    dirty_add_area(clipped_x1, clipped_y1, clipped_x2, clipped_y2);
    s_perf_stats.area_count++;
    return ESP_OK;
}

esp_err_t bsp_display_wait_flush_done(uint32_t timeout_ms)
{
    if (s_flush_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t start_tick    = xTaskGetTickCount();
    const TickType_t timeout_ticks = timeout_to_ticks(timeout_ms);

    while (true)
    {
        if (xSemaphoreTake(s_flush_mutex, timeout_ticks) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        const bool done = !s_dma_active && !s_pending_batch.valid;
        xSemaphoreGive(s_flush_mutex);
        if (done)
        {
            return ESP_OK;
        }

        if (timeout_ticks != portMAX_DELAY && (xTaskGetTickCount() - start_tick) >= timeout_ticks)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t bsp_display_request_flush(void)
{
    if (!display_accepts_frames() || s_lcd_spi == NULL || draw_framebuffer() == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const int      flush_fb_index     = s_draw_fb_index;
    const int      next_draw_fb_index = (s_draw_fb_index + 1) % RLCD_FRAMEBUFFER_COUNT;
    const uint8_t *flush_fb           = s_framebuffer[flush_fb_index];

    rlcd_flush_window_t windows[RLCD_MAX_FLUSH_WINDOWS];
    const int           win_count = make_dirty_windows(windows, RLCD_MAX_FLUSH_WINDOWS);
    if (win_count <= 0)
    {
        return ESP_OK;
    }

    /* 所有 dirty 矩形都需 copy 到下一帧 framebuffer，保持双缓冲一致。
     * window 数量可能少于 rect 数量（退化全屏或 buffer 不足合并），
     * 但 copy 覆盖范围必须和实际写入 framebuffer 的脏区一致。 */
    int64_t segment_start_us = esp_timer_get_time();
    size_t  dirty_bytes      = 0;
    for (int i = 0; i < s_dirty_count; ++i)
    {
        dirty_bytes += copy_dirty_region(s_framebuffer[next_draw_fb_index], flush_fb, &s_dirty_rects[i]);
    }
    s_perf_stats.copy_us += (uint64_t) (esp_timer_get_time() - segment_start_us);
    s_perf_stats.dirty_bytes += (uint32_t) dirty_bytes;
    dirty_reset();
    s_draw_fb_index = next_draw_fb_index;

    return submit_flush_batch(flush_fb, windows, win_count);
}

esp_err_t bsp_display_stop(uint32_t timeout_ms)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!display_accepts_frames())
    {
        return s_io_held ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    set_display_accepting_frames(false);
    const esp_err_t wait_error = bsp_display_wait_flush_done(timeout_ms);
    if (wait_error != ESP_OK)
    {
        set_display_accepting_frames(true);
        return wait_error;
    }

    const esp_err_t interrupt_error = gpio_intr_disable(BOARD_RLCD_PIN_TE);
    if (interrupt_error != ESP_OK)
    {
        set_display_accepting_frames(true);
        return interrupt_error;
    }
    s_te_interrupt_suspended   = true;

    const esp_err_t hold_error = lcd_set_io_hold(true);
    if (hold_error != ESP_OK)
    {
        (void) lcd_set_io_hold(false);
        (void) gpio_intr_enable(BOARD_RLCD_PIN_TE);
        s_te_interrupt_suspended = false;
        set_display_accepting_frames(true);
        return hold_error;
    }
    s_io_held = true;
    ESP_LOGI(TAG, "显示已停止，DMA 静止且 LCD 输出脚已保持");
    return ESP_OK;
}

esp_err_t bsp_display_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (display_accepts_frames())
    {
        return ESP_OK;
    }
    if (!s_io_held)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t hold_error = lcd_set_io_hold(false);
    if (hold_error != ESP_OK)
    {
        return hold_error;
    }
    s_io_held = false;

    if (s_te_interrupt_suspended)
    {
        const esp_err_t interrupt_error = gpio_intr_enable(BOARD_RLCD_PIN_TE);
        if (interrupt_error != ESP_OK)
        {
            (void) lcd_set_io_hold(true);
            s_io_held = true;
            return interrupt_error;
        }
        s_te_interrupt_suspended = false;
    }

    set_display_accepting_frames(true);
    ESP_LOGI(TAG, "显示已恢复并重新接受刷新");
    return ESP_OK;
}

uint32_t bsp_display_get_flush_fps(void)
{
    return s_flush_fps;
}

uint32_t bsp_display_get_total_flush_count(void)
{
    return s_total_flush_count;
}

esp_err_t bsp_display_get_info_copy(bsp_display_info_t *out_info)
{
    if (out_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_info = (bsp_display_info_t) {
        .width_pixels  = BOARD_RLCD_WIDTH,
        .height_pixels = BOARD_RLCD_HEIGHT,
    };
    return ESP_OK;
}

esp_err_t bsp_display_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_OK;
    }
    set_display_accepting_frames(false);
    /* 调用方必须先停止新的 LVGL flush；这里再确认在途 DMA 已完成。 */
    ESP_RETURN_ON_ERROR(bsp_display_wait_flush_done(1000), TAG, "等待在途 DMA 完成失败");

    if (s_io_held)
    {
        ESP_RETURN_ON_ERROR(lcd_set_io_hold(false), TAG, "反初始化前解除 LCD GPIO 保持失败");
        s_io_held = false;
    }

    /* 停刷新任务 */
    if (s_display_task != NULL)
    {
        vTaskDelete(s_display_task);
        s_display_task = NULL;
    }

    /* 移除 TE GPIO ISR：必须在释放 s_te_sem 前移除，否则 TE 触发时 ISR give 已释放的 sem 会崩溃 */
    gpio_isr_handler_remove(BOARD_RLCD_PIN_TE);

    /*
     * 释放 SPI 设备前先给 CS 打开上拉，覆盖驱动关闭 CS 输出到主动停靠之间的极短窗口，
     * 避免仍处于 Display-On 的控制器把浮空噪声解释成命令。
     */
    const esp_err_t cs_pull_error = gpio_pullup_en(BOARD_RLCD_PIN_CS);
    if (cs_pull_error != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD CS 开启停靠上拉失败: %s", esp_err_to_name(cs_pull_error));
    }

    /* 释放 SPI 设备和总线 */
    if (s_lcd_spi != NULL)
    {
        ESP_RETURN_ON_ERROR(spi_bus_remove_device(s_lcd_spi), TAG, "移除 LCD SPI 设备失败");
        s_lcd_spi = NULL;
    }
    ESP_RETURN_ON_ERROR(spi_bus_free(BOARD_RLCD_SPI_HOST), TAG, "释放 LCD SPI 总线失败");
    const esp_err_t park_error = lcd_park_io_for_light_sleep();
    if (park_error != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD 控制脚停靠失败，Light-sleep 期间可能出现闪屏: %s", esp_err_to_name(park_error));
    }

    /* 释放 framebuffer、PSRAM staging 和内部 DMA buffer */
    for (int i = 0; i < RLCD_FRAMEBUFFER_COUNT; ++i)
    {
        if (s_framebuffer[i] != NULL)
        {
            heap_caps_free(s_framebuffer[i]);
            s_framebuffer[i] = NULL;
        }
    }
    for (int i = 0; i < RLCD_STAGING_BUFFER_COUNT; ++i)
    {
        if (s_staging_buffer[i] != NULL)
        {
            heap_caps_free(s_staging_buffer[i]);
            s_staging_buffer[i] = NULL;
        }
        s_staging_locked[i] = false;
    }
    for (int i = 0; i < RLCD_DMA_BUFFER_COUNT; ++i)
    {
        if (s_dma_buffer[i] != NULL)
        {
            heap_caps_free(s_dma_buffer[i]);
            s_dma_buffer[i] = NULL;
        }
    }

    /* 释放同步对象 */
    if (s_flush_mutex != NULL)
    {
        vSemaphoreDelete(s_flush_mutex);
        s_flush_mutex = NULL;
    }
    if (s_frame_ready_sem != NULL)
    {
        vSemaphoreDelete(s_frame_ready_sem);
        s_frame_ready_sem = NULL;
    }
    if (s_te_sem != NULL)
    {
        vSemaphoreDelete(s_te_sem);
        s_te_sem = NULL;
    }

    /* 释放用于稳定 APB 频率的 SPI 总线时序锁。 */
    if (s_bus_timing_lock != NULL)
    {
        esp_pm_lock_delete(s_bus_timing_lock);
        s_bus_timing_lock = NULL;
    }

    s_initialized            = false;
    s_te_interrupt_suspended = false;
    ESP_LOGI(TAG, "显示已反初始化");
    return ESP_OK;
}
