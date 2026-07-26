/*
 * 文件职责：创建独立 SDMMC Host 和 SD 卡实例，并提供同步块设备访问。
 * 主要依赖：board_storage、ESP-IDF SDMMC Host/协议层。
 * 调用方：device_storage。
 */
#include "bsp.h"

#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"

static const char *TAG = "bsp_storage";

static bool         s_ready;
static bool         s_host_initialized;
static sdmmc_card_t s_card;

/** @brief 判断扇区范围是否落在当前卡容量内 */
static bool bsp_storage_sector_range_is_valid(uint32_t start_sector, size_t sector_count)
{
    return sector_count > 0U && (uint64_t) start_sector + (uint64_t) sector_count <= (uint64_t) s_card.csd.capacity;
}

/**
 * @brief 释放当前组件独占的 SDMMC Host 资源
 *
 * 本函数不负责加锁，只能由已经串行化生命周期的调用路径使用。释放失败时保留当前状态，
 * 使后续反初始化可以继续重试。
 *
 * @return ESP_OK 所有已取得资源均已释放；或首个底层清理错误码
 */
static esp_err_t bsp_storage_release_resources(void)
{
    if (!s_host_initialized)
    {
        return ESP_OK;
    }

    const esp_err_t error = sdmmc_host_deinit();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "释放 SDMMC Host 失败: %s", esp_err_to_name(error));
        return error;
    }

    s_host_initialized = false;
    s_ready            = false;
    memset(&s_card, 0, sizeof(s_card));
    return ESP_OK;
}

esp_err_t bsp_storage_init(void)
{
    if (s_ready)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!s_host_initialized, ESP_ERR_INVALID_STATE, TAG, "SD 卡资源处于未完成清理状态");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = BOARD_STORAGE_SDMMC_SLOT;
    host.max_freq_khz = BOARD_STORAGE_MAX_FREQ_KHZ;

    esp_err_t ret     = sdmmc_host_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 SDMMC Host 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_host_initialized              = true;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width               = BOARD_STORAGE_BUS_WIDTH;
    slot_config.clk                 = (gpio_num_t) BOARD_STORAGE_PIN_CLK;
    slot_config.cmd                 = (gpio_num_t) BOARD_STORAGE_PIN_CMD;
    slot_config.d0                  = (gpio_num_t) BOARD_STORAGE_PIN_D0;
    ret                             = sdmmc_host_init_slot(host.slot, &slot_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 SDMMC 卡槽失败: %s", esp_err_to_name(ret));
        (void) bsp_storage_release_resources();
        return ret;
    }

    ret = sdmmc_card_init(&host, &s_card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "探测并初始化 SD 卡失败: %s", esp_err_to_name(ret));
        (void) bsp_storage_release_resources();
        return ret;
    }

    s_ready = true;
    ESP_LOGI(TAG,
             "SD 卡初始化完成: SDMMC 1-bit, CLK=%d, CMD=%d, D0=%d, 容量=%llu 字节",
             BOARD_STORAGE_PIN_CLK,
             BOARD_STORAGE_PIN_CMD,
             BOARD_STORAGE_PIN_D0,
             (unsigned long long) ((uint64_t) s_card.csd.capacity * (uint64_t) s_card.csd.sector_size));
    return ESP_OK;
}

esp_err_t bsp_storage_deinit(void)
{
    if (!s_ready && !s_host_initialized)
    {
        return ESP_OK;
    }
    const esp_err_t error = bsp_storage_release_resources();
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "SD 卡板级资源已释放");
    }
    return error;
}

esp_err_t bsp_storage_check_ready(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "SD 卡尚未初始化");
    return sdmmc_get_status(&s_card);
}

esp_err_t bsp_storage_get_info_copy(bsp_storage_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(out_info != NULL, ESP_ERR_INVALID_ARG, TAG, "SD 卡信息输出为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "SD 卡尚未初始化");

    out_info->sector_count      = (uint32_t) s_card.csd.capacity;
    out_info->sector_size_bytes = (uint32_t) s_card.csd.sector_size;
    out_info->capacity_bytes    = (uint64_t) out_info->sector_count * (uint64_t) out_info->sector_size_bytes;
    return ESP_OK;
}

esp_err_t bsp_storage_read_sectors(uint32_t start_sector, size_t sector_count, void *out_data)
{
    ESP_RETURN_ON_FALSE(out_data != NULL, ESP_ERR_INVALID_ARG, TAG, "SD 卡读取缓冲区为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "SD 卡尚未初始化");
    ESP_RETURN_ON_FALSE(bsp_storage_sector_range_is_valid(start_sector, sector_count),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "SD 卡读取扇区范围无效");
    return sdmmc_read_sectors(&s_card, out_data, start_sector, sector_count);
}

esp_err_t bsp_storage_write_sectors(uint32_t start_sector, size_t sector_count, const void *data)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "SD 卡写入缓冲区为空");
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "SD 卡尚未初始化");
    ESP_RETURN_ON_FALSE(bsp_storage_sector_range_is_valid(start_sector, sector_count),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "SD 卡写入扇区范围无效");
    return sdmmc_write_sectors(&s_card, data, start_sector, sector_count);
}
