/**
 * @file connect_portal_dns_task.c
 * @brief 管理配网 Portal DNS 处理任务的完整生命周期
 */
#include "connect_internal.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
#include "task_stack_stats.h"
#endif

#define CONNECT_PORTAL_DNS_TASK_STACK_SIZE 3072U
#define CONNECT_PORTAL_DNS_TASK_PRIORITY   3U
#define CONNECT_PORTAL_DNS_STOP_TIMEOUT_MS 500U

/** @brief 日志标签 */
static const char *TAG = "connect_dns_task";

/** @brief DNS 任务退出握手信号量 */
static SemaphoreHandle_t s_dns_stopped;

/** @brief 当前 DNS 任务句柄；仅由启停接口写入 */
static TaskHandle_t s_dns_task;

/**
 * @brief 循环处理 DNS 请求，收到停止通知后进入挂起态等待控制方回收
 *
 * 任务不自行删除，避免停止方等待超时后与任务自删除竞争同一个句柄。
 *
 * @param[in] arg 未使用参数
 */
static void connect_portal_dns_task(void *arg)
{
    (void) arg;
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
    task_stack_stats_t stack_stats = TASK_STACK_STATS_INITIALIZER;
#endif

    while (ulTaskNotifyTake(pdTRUE, 0) == 0U)
    {
#if CONFIG_COMMUNICATION_TASK_STACK_STATS
        task_stack_stats_log_if_due(&stack_stats, "connect_dns");
#endif
        const esp_err_t err = connect_internal_portal_dns_process_once();
        if (err == ESP_ERR_INVALID_STATE)
        {
            break;
        }
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "处理配网 DNS 请求失败：%s", esp_err_to_name(err));
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10U)) > 0U)
            {
                break;
            }
        }
    }

#if CONFIG_COMMUNICATION_TASK_STACK_STATS
    task_stack_stats_log_now("connect_dns");
#endif
    (void) xSemaphoreGive(s_dns_stopped);
    vTaskSuspend(NULL);
}

/**
 * @brief 启动 Portal DNS socket 与处理任务
 *
 * @return ESP_OK 已启动或原本已启动；其他值表示资源创建失败
 */
esp_err_t connect_internal_portal_dns_start(void)
{
    if (s_dns_task != NULL)
    {
        return ESP_OK;
    }

    if (s_dns_stopped == NULL)
    {
        s_dns_stopped = xSemaphoreCreateBinary();
        if (s_dns_stopped == NULL)
        {
            ESP_LOGE(TAG, "创建 DNS 任务退出信号量失败");
            return ESP_ERR_NO_MEM;
        }
    }

    (void) xSemaphoreTake(s_dns_stopped, 0);
    const esp_err_t open_err = connect_internal_portal_dns_open();
    if (open_err != ESP_OK)
    {
        return open_err;
    }

    if (xTaskCreate(connect_portal_dns_task,
                    "connect_dns",
                    CONNECT_PORTAL_DNS_TASK_STACK_SIZE,
                    NULL,
                    CONNECT_PORTAL_DNS_TASK_PRIORITY,
                    &s_dns_task)
        != pdPASS)
    {
        s_dns_task = NULL;
        connect_internal_portal_dns_close();
        ESP_LOGE(TAG, "创建配网 DNS 任务失败");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief 停止 Portal DNS 处理任务并关闭 socket
 *
 * @return ESP_OK 已停止或原本未启动；ESP_ERR_TIMEOUT 表示任务未按时确认退出
 */
esp_err_t connect_internal_portal_dns_stop(void)
{
    connect_internal_portal_dns_close();

    TaskHandle_t task = s_dns_task;
    if (task == NULL)
    {
        if (s_dns_stopped != NULL)
        {
            vSemaphoreDelete(s_dns_stopped);
            s_dns_stopped = NULL;
        }
        return ESP_OK;
    }

    xTaskNotifyGive(task);
    if (xSemaphoreTake(s_dns_stopped, pdMS_TO_TICKS(CONNECT_PORTAL_DNS_STOP_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "等待配网 DNS 任务退出超时，保留任务和 socket 状态以便安全收敛");
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelete(task);
    s_dns_task = NULL;
    vSemaphoreDelete(s_dns_stopped);
    s_dns_stopped = NULL;
    return ESP_OK;
}
