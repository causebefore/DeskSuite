/**
 * @file bsp_i2c.c
 * @brief 创建、复位并引用计数管理板载共享 I2C 总线
 */
#include "bsp_i2c_internal.h"

#include <stdint.h>

#include "board.h"
#include "esp_log.h"

/** @brief ESP-IDF 推荐的毛刺过滤周期数 */
#define BSP_I2C_GLITCH_IGNORE_COUNT 7U

/** @brief 日志标签 */
static const char *TAG = "bsp_i2c";

/** @brief BSP 唯一持有的共享 I2C 总线 */
static i2c_master_bus_handle_t s_bus;
/** @brief 已成功初始化且仍持有总线的板级设备数量 */
static uint32_t s_user_count;

esp_err_t bsp_i2c_acquire(i2c_master_bus_handle_t *out_bus)
{
    if (out_bus == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_bus == NULL)
    {
        const i2c_master_bus_config_t bus_config = {
            .i2c_port                     = BOARD_I2C_PORT_NUM,
            .sda_io_num                   = BOARD_I2C_SDA_GPIO,
            .scl_io_num                   = BOARD_I2C_SCL_GPIO,
            .clk_source                   = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt            = BSP_I2C_GLITCH_IGNORE_COUNT,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t error = i2c_new_master_bus(&bus_config, &s_bus);
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "共享 I2C 总线创建失败: %s", esp_err_to_name(error));
            return error;
        }

        error = i2c_master_bus_reset(s_bus);
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "共享 I2C 总线复位失败: %s", esp_err_to_name(error));
            (void) i2c_del_master_bus(s_bus);
            s_bus = NULL;
            return error;
        }
        ESP_LOGI(TAG, "共享 I2C 总线已创建并复位，SDA=GPIO%d，SCL=GPIO%d",
                 BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    }

    ++s_user_count;
    *out_bus = s_bus;
    return ESP_OK;
}

esp_err_t bsp_i2c_release(void)
{
    if (s_bus == NULL || s_user_count == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }

    --s_user_count;
    if (s_user_count > 0U)
    {
        return ESP_OK;
    }

    const esp_err_t error = i2c_del_master_bus(s_bus);
    if (error != ESP_OK)
    {
        s_user_count = 1U;
        ESP_LOGE(TAG, "共享 I2C 总线释放失败: %s", esp_err_to_name(error));
        return error;
    }
    s_bus = NULL;
    return ESP_OK;
}
