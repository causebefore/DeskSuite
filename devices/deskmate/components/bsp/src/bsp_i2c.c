/*
 * 文件职责：封装 I2C 总线初始化、同步读写和底层总线句柄管理。
 * 主要依赖：board_pins.h、ESP-IDF I2C 驱动。
 * 调用方：bsp_audio、bsp_environment、bsp_rtc 等硬件模块。
 */
#include "bsp_i2c_internal.h"

#include <stdbool.h>

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_i2c";

#define BSP_I2C_MAX_DEVICES      6
#define BSP_I2C_TIMEOUT_MS       100
#define BSP_I2C_MUTEX_TIMEOUT_MS 200

static i2c_master_bus_handle_t s_bus;
static esp_pm_lock_handle_t    s_bus_timing_lock;
static bool                    s_ready;
/* 保护设备表与 I2C 事务的互斥锁；多任务并发访问同一总线时串行化 */
static SemaphoreHandle_t s_bus_mutex;
static StaticSemaphore_t s_mutex_buffer;
/* 保护 bsp_i2c_init 的 check-then-set，消除并发首次访问的 lazy-init 竞态 */
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;

static struct
{
    uint8_t                 addr;
    i2c_master_dev_handle_t handle;
} s_devices[BSP_I2C_MAX_DEVICES];

static size_t s_device_count;

static void acquire_bus_timing_lock(void)
{
    if (s_bus_timing_lock != NULL)
    {
        (void) esp_pm_lock_acquire(s_bus_timing_lock);
    }
}

static void release_bus_timing_lock(void)
{
    if (s_bus_timing_lock != NULL)
    {
        (void) esp_pm_lock_release(s_bus_timing_lock);
    }
}

static i2c_master_dev_handle_t get_device(uint8_t addr)
{
    for (size_t i = 0; i < s_device_count; i++)
    {
        if (s_devices[i].addr == addr)
        {
            return s_devices[i].handle;
        }
    }

    if (s_device_count >= BSP_I2C_MAX_DEVICES)
    {
        return NULL;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = BOARD_I2C_FREQ_HZ,
    };

    i2c_master_dev_handle_t handle = NULL;
    esp_err_t               err    = i2c_master_bus_add_device(s_bus, &dev_cfg, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "添加 I2C 设备失败: addr=0x%02x, err=%s", addr, esp_err_to_name(err));
        return NULL;
    }

    s_devices[s_device_count].addr   = addr;
    s_devices[s_device_count].handle = handle;
    s_device_count++;
    return handle;
}

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

    /* 慢速路径：临界区串行化初始化，确保只有一个任务真正创建总线与 mutex。
     * 临界区内操作均非阻塞：i2c_new_master_bus 配寄存器、CreateMutexStatic 用静态
     * buffer 不 malloc；动态调频未启用时，时序锁创建返回 NOT_SUPPORTED 且不分配。 */
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

    s_bus_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    if (s_bus_mutex == NULL)
    {
        portEXIT_CRITICAL(&s_init_lock);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t lock_err = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "i2c", &s_bus_timing_lock);
    if (lock_err != ESP_OK)
    {
        s_bus_timing_lock = NULL;
    }

    s_ready = true;
    portEXIT_CRITICAL(&s_init_lock);

    if (lock_err != ESP_OK)
    {
        ESP_LOGW(TAG, "创建 I2C 总线时序锁失败: %s", esp_err_to_name(lock_err));
    }
    ESP_LOGI(TAG,
             "I2C 初始化完成: port=%d, SDA=%d, SCL=%d, freq=%d",
             BOARD_I2C_PORT,
             BOARD_I2C_PIN_SDA,
             BOARD_I2C_PIN_SCL,
             BOARD_I2C_FREQ_HZ);
    return ESP_OK;
}

esp_err_t bsp_i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    if (data == NULL && len > 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "初始化 I2C 失败");
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(BSP_I2C_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    i2c_master_dev_handle_t dev = get_device(addr);
    esp_err_t               err = ESP_ERR_NO_MEM;
    if (dev != NULL)
    {
        acquire_bus_timing_lock();
        err = i2c_master_transmit(dev, data, len, BSP_I2C_TIMEOUT_MS);
        release_bus_timing_lock();
    }
    else
    {
        ESP_LOGW(TAG, "获取 I2C 设备失败: addr=0x%02x", addr);
    }

    xSemaphoreGive(s_bus_mutex);
    return err;
}

esp_err_t bsp_i2c_read(uint8_t addr, uint8_t *data, size_t len)
{
    if (data == NULL && len > 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "初始化 I2C 失败");
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(BSP_I2C_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    i2c_master_dev_handle_t dev = get_device(addr);
    esp_err_t               err = ESP_ERR_NO_MEM;
    if (dev != NULL)
    {
        acquire_bus_timing_lock();
        err = i2c_master_receive(dev, data, len, BSP_I2C_TIMEOUT_MS);
        release_bus_timing_lock();
    }
    else
    {
        ESP_LOGW(TAG, "获取 I2C 设备失败: addr=0x%02x", addr);
    }

    xSemaphoreGive(s_bus_mutex);
    return err;
}

esp_err_t bsp_i2c_write_read(uint8_t addr, const uint8_t *write_data, size_t write_len, uint8_t *read_data,
                             size_t read_len)
{
    if ((write_data == NULL && write_len > 0) || (read_data == NULL && read_len > 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "初始化 I2C 失败");
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(BSP_I2C_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    i2c_master_dev_handle_t dev = get_device(addr);
    esp_err_t               err = ESP_ERR_NO_MEM;
    if (dev != NULL)
    {
        acquire_bus_timing_lock();
        err = i2c_master_transmit_receive(dev, write_data, write_len, read_data, read_len, BSP_I2C_TIMEOUT_MS);
        release_bus_timing_lock();
    }
    else
    {
        ESP_LOGW(TAG, "获取 I2C 设备失败: addr=0x%02x", addr);
    }

    xSemaphoreGive(s_bus_mutex);
    return err;
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
    s_ready             = false;
    size_t device_count = s_device_count;
    portEXIT_CRITICAL(&s_init_lock);

    /* 释放动态添加的 I2C 设备句柄（必须先于总线删除，否则资源泄漏） */
    for (size_t i = 0; i < device_count; i++)
    {
        if (s_devices[i].handle != NULL)
        {
            i2c_master_bus_rm_device(s_devices[i].handle);
            s_devices[i].handle = NULL;
        }
    }
    s_device_count = 0;

    /* 删除 I2C master 总线 */
    if (s_bus != NULL)
    {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }

    /* 释放总线互斥锁：xSemaphoreCreateMutexStatic 创建，vSemaphoreDelete 仅释放
     * 信号量对象本身，静态 buffer(s_mutex_buffer) 无需单独 free */
    if (s_bus_mutex != NULL)
    {
        vSemaphoreDelete(s_bus_mutex);
        s_bus_mutex = NULL;
    }

    /* 释放用于稳定 APB 频率的 I2C 总线时序锁。 */
    if (s_bus_timing_lock != NULL)
    {
        esp_pm_lock_delete(s_bus_timing_lock);
        s_bus_timing_lock = NULL;
    }

    ESP_LOGI(TAG, "I2C 反初始化完成");
    return ESP_OK;
}
