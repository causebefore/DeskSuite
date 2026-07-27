/**
 * @file time_sync.c
 * @brief 实现串行、单次的 SNTP 网络取样
 */
#include "time_sync.h"

#include <stdbool.h>
#include <string.h>

#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** @brief 静态互斥量存储，避免取样路径动态分配 */
static StaticSemaphore_t s_sample_mutex_storage;
/** @brief 保护 ESP-NETIF 全局 SNTP 客户端生命周期 */
static SemaphoreHandle_t s_sample_mutex;
/** @brief 保护互斥量首次构造 */
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

/** @brief 幂等取得 SNTP 全局客户端互斥量 */
static SemaphoreHandle_t time_sync_get_sample_mutex(void)
{
    taskENTER_CRITICAL(&s_mutex_init_lock);
    if (s_sample_mutex == NULL)
    {
        s_sample_mutex = xSemaphoreCreateMutexStatic(&s_sample_mutex_storage);
    }
    SemaphoreHandle_t mutex = s_sample_mutex;
    taskEXIT_CRITICAL(&s_mutex_init_lock);
    return mutex;
}

esp_err_t time_sync_sample_sntp_once_copy(const char *server, uint32_t timeout_ms, time_sync_sample_t *out_sample)
{
    if (server == NULL || server[0] == '\0' || timeout_ms == 0U || out_sample == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_sample, 0, sizeof(*out_sample));

    SemaphoreHandle_t mutex = time_sync_get_sample_mutex();
    if (mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    const TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(mutex, wait_ticks > 0U ? wait_ticks : 1U) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    const int64_t     started_us = esp_timer_get_time();
    esp_sntp_config_t config     = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    esp_err_t         result     = esp_netif_sntp_init(&config);
    if (result == ESP_OK)
    {
        result = esp_netif_sntp_sync_wait(wait_ticks > 0U ? wait_ticks : 1U);
    }

    time_t timestamp = 0;
    if (result == ESP_OK && time(&timestamp) == (time_t) -1)
    {
        result = ESP_FAIL;
    }
    esp_netif_sntp_deinit();

    if (result == ESP_OK)
    {
        const int64_t elapsed_us  = esp_timer_get_time() - started_us;
        out_sample->utc_timestamp = timestamp;
        out_sample->elapsed_ms    = elapsed_us > 0 ? (uint32_t) ((elapsed_us + 999LL) / 1000LL) : 0U;
    }
    xSemaphoreGive(mutex);
    return result;
}
