/*
 * 文件职责：固件主入口，显式初始化系统基础能力并启动 App。
 * 主要依赖：device_rtc、device_storage、sys、app_main、ESP-IDF Heap/Timer。
 * 调用方：ESP-IDF 启动流程。
 */
#include "app_main.h"
#include "device_rtc.h"
#include "device_storage.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings_store.h"
#include "system_clock.h"
#include "system_filesystem.h"
#include "system_storage.h"
#include "task_stack_stats.h"

#define MEMORY_STATS_LOG_TAG     "memory_stats"
#define MEMORY_STATS_INTERVAL_US (10ULL * 1000ULL * 1000ULL)

static const char        *TAG = "main";
static esp_timer_handle_t s_memory_stats_timer;

/** @brief 输出内部 SRAM 总览 */
static void memory_stats_timer_callback(void *arg)
{
    (void) arg;
    const uint32_t capabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t   total_bytes  = heap_caps_get_total_size(capabilities);
    const size_t   free_bytes   = heap_caps_get_free_size(capabilities);
    const size_t   used_bytes   = total_bytes >= free_bytes ? total_bytes - free_bytes : 0U;
    const uint32_t used_permille =
        total_bytes > 0U ? (uint32_t) ((used_bytes * 1000U + total_bytes / 2U) / total_bytes) : 0U;

    ESP_LOGI(MEMORY_STATS_LOG_TAG,
             "内部 SRAM: 占用=%lu.%lu%%, 已用=%lu 字节, 空闲=%lu 字节, "
             "历史最小空闲=%lu 字节, 最大连续块=%lu 字节",
             (unsigned long) (used_permille / 10U),
             (unsigned long) (used_permille % 10U),
             (unsigned long) used_bytes,
             (unsigned long) free_bytes,
             (unsigned long) heap_caps_get_minimum_free_size(capabilities),
             (unsigned long) heap_caps_get_largest_free_block(capabilities));
}

/** @brief 在产品初始化前启动 10 秒 SRAM 总览 */
static void start_memory_stats_logging(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = memory_stats_timer_callback,
        .name     = MEMORY_STATS_LOG_TAG,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_memory_stats_timer);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "创建内存统计定时器失败: %s", esp_err_to_name(err));
        return;
    }

    memory_stats_timer_callback(NULL);
    err = esp_timer_start_periodic(s_memory_stats_timer, MEMORY_STATS_INTERVAL_US);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "启动内存统计定时器失败: %s", esp_err_to_name(err));
        (void) esp_timer_delete(s_memory_stats_timer);
        s_memory_stats_timer = NULL;
    }
}

/**
 * @brief 同步装配可选 SD 卡与 FAT 文件系统，失败时回滚并降级继续
 *
 * Device 成功而文件系统失败时，先清理文件系统注册，再释放
 * Device。若文件系统仍持有资源， 会保留 Device，避免残留 DiskIO
 * 回调访问已经释放的块设备。本函数不把可选存储失败传播为 整机启动失败。
 */
static void init_optional_sd_filesystem(void)
{
    esp_err_t error = device_storage_init();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "可选 SD 卡初始化失败，将继续启动: %s", esp_err_to_name(error));
        return;
    }

    error = system_filesystem_init();
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "可选 SD 卡文件系统挂载失败，将继续启动: %s", esp_err_to_name(error));
        const esp_err_t filesystem_cleanup_error = system_filesystem_deinit();
        if (filesystem_cleanup_error != ESP_OK && filesystem_cleanup_error != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG,
                     "回滚 SD 卡文件系统资源失败，将保留 Device 避免悬空引用: %s",
                     esp_err_to_name(filesystem_cleanup_error));
            return;
        }
        const esp_err_t cleanup_error = device_storage_deinit();
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGW(TAG, "回滚 SD 卡 Device 资源失败: %s", esp_err_to_name(cleanup_error));
        }
        return;
    }

    system_filesystem_info_t info;
    error = system_filesystem_get_info_copy(&info);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "读取 SD 卡文件系统容量失败: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(TAG,
             "SD 卡文件系统就绪: 挂载点=%s, 总容量=%llu 字节, 可用=%llu 字节, "
             "本次格式化=%s",
             SYSTEM_FILESYSTEM_MOUNT_POINT,
             (unsigned long long) info.total_bytes,
             (unsigned long long) info.free_bytes,
             info.formatted_during_init ? "是" : "否");
}

/**
 * @brief ESP-IDF 固件入口，按明确顺序初始化基础能力和产品流程
 */
void app_main(void)
{
    // esp_log_level_set("*", ESP_LOG_WARN);
    // esp_log_level_set(TAG, ESP_LOG_INFO);
    // esp_log_level_set(TASK_STACK_STATS_LOG_TAG, ESP_LOG_INFO);
    // esp_log_level_set(MEMORY_STATS_LOG_TAG, ESP_LOG_INFO);
    //   start_memory_stats_logging();

    esp_err_t err = system_storage_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "系统存储初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = settings_store_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设备设置初始化失败: %s", esp_err_to_name(err));
        return;
    }

    init_optional_sd_filesystem();

    err = device_rtc_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "设备 RTC 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = system_clock_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "系统时钟初始化失败: %s", esp_err_to_name(err));
        return;
    }
    err = system_clock_sync_from_rtc();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "RTC 时间当前不可用，等待联网后通过 SNTP 校时: %s", esp_err_to_name(err));
    }

    err = app_main_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "App 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = app_main_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "App 启动失败: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "DeskMate 启动完成");
}
