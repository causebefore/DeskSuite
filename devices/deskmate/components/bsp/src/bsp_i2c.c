/*
 * 文件职责：封装 I2C 总线初始化和底层总线句柄管理。
 * 主要依赖：board_pins.h、ESP-IDF I2C 驱动。
 * 调用方：bsp_audio、bsp_environment、bsp_rtc 等硬件模块。
 */
#include "bsp_i2c_internal.h"

#include <stdbool.h>

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bsp_i2c";

static i2c_master_bus_handle_t s_bus;
static bool                    s_ready;
/* 保护 bsp_i2c_init 的 check-then-set，消除并发首次访问的 lazy-init 竞态 */
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t bsp_i2c_init(void)
{
    /* 快速路径：已就绪直接返回 */
    portENTER_CRITICAL(&s_init_lock);
    bool ready = s_ready;
    portEXIT_CRITICAL(&s_init_lock);
    if (ready)
    {
        return ESP_OK;
    }

    /* 慢速路径：临界区串行化初始化，确保只有一个任务真正创建总线。 */
    portENTER_CRITICAL(&s_init_lock);
    if (s_ready)
    {
        portEXIT_CRITICAL(&s_init_lock);
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = BOARD_I2C_PORT,
        .sda_io_num                   = BOARD_I2C_PIN_SDA,
        .scl_io_num                   = BOARD_I2C_PIN_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK)
    {
        portEXIT_CRITICAL(&s_init_lock);
        ESP_LOGE(TAG, "创建 I2C master 总线失败: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    portEXIT_CRITICAL(&s_init_lock);

    ESP_LOGI(TAG,
             "I2C 初始化完成: port=%d, SDA=%d, SCL=%d, freq=%d",
             BOARD_I2C_PORT,
             BOARD_I2C_PIN_SDA,
             BOARD_I2C_PIN_SCL,
             BOARD_I2C_FREQ_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void)
{
    return s_ready ? s_bus : NULL;
}

esp_err_t bsp_i2c_deinit(void)
{
    /* 临界区内置 s_ready=false，阻止新事务触发 lazy-init 重新进入慢速路径 */
    portENTER_CRITICAL(&s_init_lock);
    if (!s_ready)
    {
        portEXIT_CRITICAL(&s_init_lock);
        return ESP_OK;
    }
    s_ready = false;
    portEXIT_CRITICAL(&s_init_lock);

    /* 删除 I2C master 总线 */
    if (s_bus != NULL)
    {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }

    ESP_LOGI(TAG, "I2C 反初始化完成");
    return ESP_OK;
}
