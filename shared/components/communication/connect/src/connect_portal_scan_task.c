/**
 * @file connect_portal_scan_task.c
 * @brief 执行配网 Portal 的后台 Wi-Fi 扫描并维护结果缓存
 */
#include "connect_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
#include "task_stack_stats.h"
#endif
#include "utils.h"

#define CONNECT_PORTAL_SCAN_MAX_APS         12U
#define CONNECT_PORTAL_SCAN_TASK_STACK      4096U
#define CONNECT_PORTAL_SCAN_TASK_PRIO       3U
#define CONNECT_PORTAL_SCAN_STOP_TIMEOUT_MS 2000U

/** @brief 日志标签 */
static const char *TAG = "connect_scan_task";

/** @brief 扫描任务是否正在运行 */
static bool s_scan_in_progress;

/** @brief 当前扫描任务句柄，由扫描启停接口负责回收 */
static TaskHandle_t s_scan_task;

/** @brief 扫描任务退出握手信号量 */
static SemaphoreHandle_t s_scan_stopped;

/** @brief 最近一次扫描结果的 JSON 缓存 */
static char s_scan_cache[CONNECT_INTERNAL_PORTAL_SCAN_RESULT_MAX] = "[]";

/** @brief 扫描状态和缓存的临界区锁 */
static portMUX_TYPE s_scan_lock                                   = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 向 JSON 缓冲区追加十进制整数
 *
 * @param[out] out 输出缓冲区
 * @param[in] out_len 输出缓冲区容量
 * @param[in,out] offset 当前写入位置
 * @param[in] value 待追加整数
 */
static void append_number(char *out, size_t out_len, size_t *offset, int value)
{
    char text[16];
    (void) snprintf(text, sizeof(text), "%d", value);
    (void) utils_append_string(out, out_len, offset, text);
}

/**
 * @brief 将文本按 JSON 字符串规则转义后追加到缓冲区
 *
 * @param[out] out 输出缓冲区
 * @param[in] out_len 输出缓冲区容量
 * @param[in,out] offset 当前写入位置
 * @param[in] text 待转义文本
 */
static void append_json_escaped(char *out, size_t out_len, size_t *offset, const char *text)
{
    if (text == NULL)
    {
        return;
    }

    for (size_t i = 0; text[i] != '\0'; ++i)
    {
        switch (text[i])
        {
            case '\\':
                (void) utils_append_string(out, out_len, offset, "\\\\");
                break;
            case '"':
                (void) utils_append_string(out, out_len, offset, "\\\"");
                break;
            case '\n':
                (void) utils_append_string(out, out_len, offset, "\\n");
                break;
            case '\r':
                (void) utils_append_string(out, out_len, offset, "\\r");
                break;
            case '\t':
                (void) utils_append_string(out, out_len, offset, "\\t");
                break;
            default: {
                char ch[2] = { text[i], '\0' };
                (void) utils_append_string(out, out_len, offset, ch);
                break;
            }
        }
    }
}

/**
 * @brief 判断扫描记录是否与之前的可见 SSID 重复
 *
 * @param[in] records 扫描记录数组
 * @param[in] index 当前记录下标
 * @return true 已出现相同 SSID；false 未重复
 */
static bool scan_result_is_duplicate(const wifi_ap_record_t *records, uint16_t index)
{
    for (uint16_t i = 0; i < index; ++i)
    {
        if (strcmp((const char *) records[i].ssid, (const char *) records[index].ssid) == 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 执行一次 Wi-Fi 扫描并生成 JSON 缓存
 *
 * @param[out] response JSON 输出缓冲区
 * @param[in] response_len 输出缓冲区容量
 */
static void portal_collect_scan(char *response, size_t response_len)
{
    wifi_ap_record_t *records        = calloc(CONNECT_PORTAL_SCAN_MAX_APS, sizeof(*records));
    uint16_t          ap_count       = CONNECT_PORTAL_SCAN_MAX_APS;
    size_t            offset         = 0;
    bool              has_visible_ap = false;

    if (response == NULL || response_len == 0U)
    {
        free(records);
        return;
    }
    response[0] = '\0';
    if (records == NULL)
    {
        utils_copy_string(response, response_len, "[]");
        return;
    }

    const wifi_scan_config_t scan_config = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK)
    {
        err = esp_wifi_scan_get_ap_records(&ap_count, records);
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "扫描 Wi-Fi 失败：%s", esp_err_to_name(err));
        free(records);
        utils_copy_string(response, response_len, "[]");
        return;
    }

    (void) utils_append_string(response, response_len, &offset, "[");
    for (uint16_t i = 0; i < ap_count; ++i)
    {
        if (records[i].ssid[0] == '\0' || scan_result_is_duplicate(records, i))
        {
            continue;
        }

        if (has_visible_ap)
        {
            (void) utils_append_string(response, response_len, &offset, ",");
        }
        has_visible_ap = true;
        (void) utils_append_string(response, response_len, &offset, "{\"ssid\":\"");
        append_json_escaped(response, response_len, &offset, (const char *) records[i].ssid);
        (void) utils_append_string(response, response_len, &offset, "\",\"rssi\":");
        append_number(response, response_len, &offset, records[i].rssi);
        (void) utils_append_string(response, response_len, &offset, ",\"open\":");
        (void) utils_append_string(response,
                                   response_len,
                                   &offset,
                                   records[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        (void) utils_append_string(response, response_len, &offset, "}");
    }
    (void) utils_append_string(response, response_len, &offset, "]");

    free(records);
}

/**
 * @brief 在后台执行 Wi-Fi 扫描并更新缓存
 *
 * @param[in] arg 未使用参数
 */
static void connect_portal_scan_task(void *arg)
{
    (void) arg;
    char result[CONNECT_INTERNAL_PORTAL_SCAN_RESULT_MAX] = { 0 };
    portal_collect_scan(result, sizeof(result));

    taskENTER_CRITICAL(&s_scan_lock);
    utils_copy_string(s_scan_cache, sizeof(s_scan_cache), result);
    s_scan_in_progress = false;
    taskEXIT_CRITICAL(&s_scan_lock);
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
    task_stack_stats_log_now("portal_scan");
#endif
    (void) xSemaphoreGive(s_scan_stopped);
    vTaskSuspend(NULL);
}

/**
 * @brief 请求启动一次 Portal 后台 Wi-Fi 扫描
 *
 * @return ESP_OK 已运行或已启动；其他值表示任务创建失败
 */
esp_err_t connect_internal_portal_scan_start(void)
{
    taskENTER_CRITICAL(&s_scan_lock);
    const bool   already_scanning = s_scan_in_progress;
    TaskHandle_t completed_task   = already_scanning ? NULL : s_scan_task;
    if (completed_task != NULL)
    {
        s_scan_task = NULL;
    }
    taskEXIT_CRITICAL(&s_scan_lock);
    if (already_scanning)
    {
        return ESP_OK;
    }

    if (completed_task != NULL)
    {
        vTaskDelete(completed_task);
    }
    if (s_scan_stopped == NULL)
    {
        s_scan_stopped = xSemaphoreCreateBinary();
        if (s_scan_stopped == NULL)
        {
            ESP_LOGE(TAG, "创建扫描任务退出信号量失败");
            return ESP_ERR_NO_MEM;
        }
    }
    (void) xSemaphoreTake(s_scan_stopped, 0);

    taskENTER_CRITICAL(&s_scan_lock);
    s_scan_in_progress = true;
    taskEXIT_CRITICAL(&s_scan_lock);

    if (xTaskCreate(connect_portal_scan_task,
                    "portal_scan",
                    CONNECT_PORTAL_SCAN_TASK_STACK,
                    NULL,
                    CONNECT_PORTAL_SCAN_TASK_PRIO,
                    &s_scan_task)
        != pdPASS)
    {
        taskENTER_CRITICAL(&s_scan_lock);
        s_scan_in_progress = false;
        s_scan_task        = NULL;
        taskEXIT_CRITICAL(&s_scan_lock);
        vSemaphoreDelete(s_scan_stopped);
        s_scan_stopped = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief 同步停止正在运行的 Portal Wi-Fi 扫描并回收扫描任务
 *
 * @return ESP_OK 已停止或原本未启动；ESP_ERR_TIMEOUT 表示任务未按时退出
 */
esp_err_t connect_internal_portal_scan_stop(void)
{
    taskENTER_CRITICAL(&s_scan_lock);
    const bool   scan_in_progress = s_scan_in_progress;
    TaskHandle_t task             = s_scan_task;
    taskEXIT_CRITICAL(&s_scan_lock);

    if (task == NULL)
    {
        if (s_scan_stopped != NULL)
        {
            vSemaphoreDelete(s_scan_stopped);
            s_scan_stopped = NULL;
        }
        return ESP_OK;
    }

    if (scan_in_progress)
    {
        const esp_err_t scan_err = esp_wifi_scan_stop();
        if (scan_err != ESP_OK && scan_err != ESP_ERR_WIFI_NOT_STARTED && scan_err != ESP_ERR_WIFI_NOT_INIT
            && scan_err != ESP_ERR_WIFI_STATE)
        {
            ESP_LOGW(TAG, "请求停止 Wi-Fi 扫描失败：%s", esp_err_to_name(scan_err));
        }
    }

    if (xSemaphoreTake(s_scan_stopped, pdMS_TO_TICKS(CONNECT_PORTAL_SCAN_STOP_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "等待 Portal 扫描任务退出超时，保留任务和 Wi-Fi Driver 以避免破坏驱动状态");
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelete(task);
    taskENTER_CRITICAL(&s_scan_lock);
    if (s_scan_task == task)
    {
        s_scan_task = NULL;
    }
    s_scan_in_progress = false;
    taskEXIT_CRITICAL(&s_scan_lock);

    vSemaphoreDelete(s_scan_stopped);
    s_scan_stopped = NULL;
    return ESP_OK;
}

/** @brief 清空 Portal Wi-Fi 扫描缓存 */
void connect_internal_portal_scan_reset(void)
{
    taskENTER_CRITICAL(&s_scan_lock);
    utils_copy_string(s_scan_cache, sizeof(s_scan_cache), "[]");
    taskEXIT_CRITICAL(&s_scan_lock);
}

/**
 * @brief 原子复制 Portal Wi-Fi 扫描状态与缓存
 *
 * @param[out] out_cache 扫描结果 JSON 缓冲区
 * @param[in] out_cache_size 扫描结果缓冲区容量
 * @param[out] out_scanning 当前是否正在扫描
 */
void connect_internal_portal_scan_get(char *out_cache, size_t out_cache_size, bool *out_scanning)
{
    if (out_cache == NULL || out_cache_size == 0U || out_scanning == NULL)
    {
        return;
    }

    taskENTER_CRITICAL(&s_scan_lock);
    *out_scanning = s_scan_in_progress;
    utils_copy_string(out_cache, out_cache_size, s_scan_cache);
    taskEXIT_CRITICAL(&s_scan_lock);
}
