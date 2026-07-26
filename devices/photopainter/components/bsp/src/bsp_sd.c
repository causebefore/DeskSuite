/**
 * @file bsp_sd.c
 * @brief 装配 reTerminal E1001 的 SD 卡槽、共享 SPI2 总线与 FATFS
 */
#include "bsp.h"
#include "bsp_spi.h"

#include <inttypes.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define BSP_SD_SPI_HOST          SPI2_HOST
#define BSP_SD_SPI_CLOCK_KHZ     10000
#define BSP_SD_MAX_OPEN_FILES    4
#define BSP_SD_POWER_DELAY_MS    10U

/** @brief 日志标签 */
static const char *TAG = "bsp_sd";

/** @brief FATFS 挂载返回的 SD 卡句柄 */
static sdmmc_card_t *s_card;

/** @brief 本模块是否持有共享 SPI2 总线引用 */
static bool s_spi_bus_acquired;

/** @brief SD 卡槽硬件是否已初始化 */
static bool s_initialized;

/** @brief FATFS 是否已挂载 */
static bool s_mounted;

/** @brief GPIO ISR handler 是否已经注册 */
static bool s_detect_handler_registered;

/** @brief 上层借用的卡检测 ISR 回调及上下文 */
static bsp_sd_detect_isr_cb_t s_detect_callback;
static void                  *s_detect_context;

/** @brief 把 GPIO 中断转换为不携带板级细节的 SD 检测回调 */
static void bsp_sd_detect_isr_handler(void *context)
{
    (void) context;
    if (s_detect_callback != NULL)
    {
        s_detect_callback(s_detect_context);
    }
}

/** @brief 按板级有效电平开启或关闭 SD 卡槽供电 */
static esp_err_t bsp_sd_set_power(bool enabled)
{
    const uint32_t active_level = BOARD_SD_EN_ACTIVE_HIGH ? 1U : 0U;
    return gpio_set_level(BOARD_SD_EN_GPIO, enabled ? active_level : 1U - active_level);
}

/** @brief 回滚初始化期间已取得的 GPIO 和 SPI 资源 */
static void bsp_sd_rollback_init(void)
{
    if (s_spi_bus_acquired)
    {
        const esp_err_t error = bsp_spi_release();
        if (error == ESP_OK)
        {
            s_spi_bus_acquired = false;
        }
        else
        {
            ESP_LOGE(TAG, "回滚共享 SPI2 总线引用失败: %s", esp_err_to_name(error));
        }
    }
    (void) bsp_sd_set_power(false);
    (void) gpio_reset_pin(BOARD_SD_DET_GPIO);
    (void) gpio_reset_pin(BOARD_SD_CS_GPIO);
    (void) gpio_reset_pin(BOARD_SD_EN_GPIO);
}

esp_err_t bsp_sd_init(void)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* SD 规范探测会发送可选 CMD5；隐藏第三方 INFO 噪声，但保留警告与错误。 */
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);

    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << BOARD_SD_EN_GPIO) | (1ULL << BOARD_SD_CS_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&output_config);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "配置 SD 卡槽输出 GPIO 失败: %s", esp_err_to_name(error));
        return error;
    }
    (void) gpio_set_level(BOARD_SD_CS_GPIO, 1U);
    error = bsp_sd_set_power(true);
    if (error != ESP_OK)
    {
        bsp_sd_rollback_init();
        ESP_LOGE(TAG, "开启 SD 卡槽供电失败: %s", esp_err_to_name(error));
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(BSP_SD_POWER_DELAY_MS));

    const gpio_config_t detect_config = {
        .pin_bit_mask = (1ULL << BOARD_SD_DET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    error = gpio_config(&detect_config);
    if (error != ESP_OK)
    {
        bsp_sd_rollback_init();
        ESP_LOGE(TAG, "配置 SD 卡检测 GPIO 失败: %s", esp_err_to_name(error));
        return error;
    }

    error = bsp_spi_acquire();
    s_spi_bus_acquired = error == ESP_OK;
    if (error != ESP_OK)
    {
        bsp_sd_rollback_init();
        ESP_LOGE(TAG, "初始化 SD 与墨水屏共享 SPI2 总线失败: %s", esp_err_to_name(error));
        return error;
    }

    error = gpio_install_isr_service(0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        bsp_sd_rollback_init();
        ESP_LOGE(TAG, "安装 GPIO ISR 服务失败: %s", esp_err_to_name(error));
        return error;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "SD 卡槽初始化完成，检测 GPIO%d 低电平表示插卡", (int) BOARD_SD_DET_GPIO);
    return ESP_OK;
}

esp_err_t bsp_sd_set_detect_isr_callback_borrow(bsp_sd_detect_isr_cb_t callback, void *context)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    (void) gpio_intr_disable(BOARD_SD_DET_GPIO);
    if (s_detect_handler_registered)
    {
        const esp_err_t error = gpio_isr_handler_remove(BOARD_SD_DET_GPIO);
        if (error != ESP_OK)
        {
            return error;
        }
        s_detect_handler_registered = false;
    }

    s_detect_callback = callback;
    s_detect_context  = callback != NULL ? context : NULL;
    if (callback == NULL)
    {
        return ESP_OK;
    }

    esp_err_t error = gpio_isr_handler_add(BOARD_SD_DET_GPIO, bsp_sd_detect_isr_handler, NULL);
    if (error != ESP_OK)
    {
        s_detect_callback = NULL;
        s_detect_context  = NULL;
        return error;
    }
    s_detect_handler_registered = true;
    error                       = gpio_intr_enable(BOARD_SD_DET_GPIO);
    if (error != ESP_OK)
    {
        (void) gpio_isr_handler_remove(BOARD_SD_DET_GPIO);
        s_detect_handler_registered = false;
        s_detect_callback           = NULL;
        s_detect_context            = NULL;
    }
    return error;
}

esp_err_t bsp_sd_is_card_present(bool *out_present)
{
    if (out_present == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const int level = gpio_get_level(BOARD_SD_DET_GPIO);
    *out_present    = BOARD_SD_DET_ACTIVE_LOW ? level == 0 : level != 0;
    return ESP_OK;
}

esp_err_t bsp_sd_mount(void)
{
    if (!s_initialized || s_mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool      present = false;
    esp_err_t error   = bsp_sd_is_card_present(&present);
    if (error != ESP_OK)
    {
        return error;
    }
    if (!present)
    {
        return ESP_ERR_NOT_FOUND;
    }

    sdmmc_host_t host                             = SDSPI_HOST_DEFAULT();
    host.slot                                     = BSP_SD_SPI_HOST;
    host.max_freq_khz                             = BSP_SD_SPI_CLOCK_KHZ;

    sdspi_device_config_t slot_config             = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id                           = BSP_SD_SPI_HOST;
    slot_config.gpio_cs                           = BOARD_SD_CS_GPIO;
    slot_config.gpio_cd                           = SDSPI_SLOT_NO_CD;

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed   = false,
        .max_files                = BSP_SD_MAX_OPEN_FILES,
        .allocation_unit_size     = 0U,
        .disk_status_check_enable = true,
        .use_one_fat              = false,
    };
    error =
        esp_vfs_fat_sdspi_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (error != ESP_OK)
    {
        s_card = NULL;
        ESP_LOGE(TAG, "挂载 SD 卡 FATFS 失败: %s", esp_err_to_name(error));
        return error;
    }

    s_mounted                     = true;
    const uint64_t capacity_bytes = (uint64_t) s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG,
             "SD 卡 FATFS 已挂载到 %s，容量约 %" PRIu64 " MiB",
             BSP_SD_MOUNT_POINT,
             capacity_bytes / (1024U * 1024U));
    return ESP_OK;
}

esp_err_t bsp_sd_unmount(void)
{
    if (!s_initialized || !s_mounted || s_card == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t error = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "卸载 SD 卡 FATFS 失败: %s", esp_err_to_name(error));
        return error;
    }

    s_card    = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "SD 卡 FATFS 已卸载，共享 SPI2 总线继续保留");
    return ESP_OK;
}

esp_err_t bsp_sd_deinit(void)
{
    if (!s_initialized || s_mounted || s_detect_handler_registered || s_detect_callback != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_spi_bus_acquired)
    {
        const esp_err_t error = bsp_spi_release();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "释放共享 SPI2 总线失败: %s", esp_err_to_name(error));
            return error;
        }
        s_spi_bus_acquired = false;
    }

    esp_err_t        first_error = bsp_sd_set_power(false);
    const gpio_num_t pins[]      = { BOARD_SD_DET_GPIO, BOARD_SD_CS_GPIO, BOARD_SD_EN_GPIO };
    for (size_t index = 0U; index < sizeof(pins) / sizeof(pins[0]); ++index)
    {
        const esp_err_t error = gpio_reset_pin(pins[index]);
        if (first_error == ESP_OK && error != ESP_OK)
        {
            first_error = error;
        }
    }
    if (first_error != ESP_OK)
    {
        ESP_LOGE(TAG, "释放 SD 卡槽 GPIO 失败: %s", esp_err_to_name(first_error));
        return first_error;
    }

    s_initialized = false;
    return ESP_OK;
}
