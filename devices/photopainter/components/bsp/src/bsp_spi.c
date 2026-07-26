/**
 * @file bsp_spi.c
 * @brief 统一拥有 SD 卡与墨水屏共用的 SPI2 总线
 */
#include "bsp_spi.h"

#include <stdint.h>

#include "board.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#define BSP_SPI_HOST              SPI2_HOST
#define BSP_SPI_MAX_TRANSFER_SIZE 4096

_Static_assert(BOARD_SD_MOSI_GPIO == BOARD_EPD_MOSI_GPIO,
               "SD 卡与墨水屏必须共用同一 MOSI GPIO");
_Static_assert(BOARD_SD_SCK_GPIO == BOARD_EPD_SCK_GPIO,
               "SD 卡与墨水屏必须共用同一 SCK GPIO");

/** @brief 日志标签 */
static const char *TAG = "bsp_spi";

/** @brief 当前由 SD 卡与墨水屏持有的共享总线引用数 */
static uint32_t s_reference_count;

esp_err_t bsp_spi_acquire(void)
{
    if (s_reference_count == UINT32_MAX)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_reference_count > 0U)
    {
        ++s_reference_count;
        ESP_LOGI(TAG,
                 "复用已初始化的共享 SPI2 总线: 引用数=%lu",
                 (unsigned long) s_reference_count);
        return ESP_OK;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num     = BOARD_SD_MOSI_GPIO,
        .miso_io_num     = BOARD_SD_MISO_GPIO,
        .sclk_io_num     = BOARD_SD_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BSP_SPI_MAX_TRANSFER_SIZE,
    };
    const esp_err_t error =
        spi_bus_initialize(BSP_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 SD 与墨水屏共享 SPI2 总线失败: %s", esp_err_to_name(error));
        return error;
    }

    s_reference_count = 1U;
    ESP_LOGI(TAG, "共享 SPI2 总线初始化完成: 引用数=1");
    return ESP_OK;
}

esp_err_t bsp_spi_release(void)
{
    if (s_reference_count == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_reference_count > 1U)
    {
        --s_reference_count;
        ESP_LOGI(TAG,
                 "释放共享 SPI2 总线引用: 引用数=%lu",
                 (unsigned long) s_reference_count);
        return ESP_OK;
    }

    const esp_err_t error = spi_bus_free(BSP_SPI_HOST);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "关闭共享 SPI2 总线失败: %s", esp_err_to_name(error));
        return error;
    }
    s_reference_count = 0U;
    ESP_LOGI(TAG, "共享 SPI2 总线已关闭: 引用数=0");
    return ESP_OK;
}
