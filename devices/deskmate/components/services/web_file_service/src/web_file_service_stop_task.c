/**
 * @file web_file_service_stop_task.c
 * @brief 在一次性 Task 中执行无超时的 ESP-IDF HTTPD 合法销毁流程
 */
#include "web_file_service_internal.h"

#include "esp_log.h"

#define WEB_FILE_HTTPD_STOP_TASK_STACK_SIZE_BYTES 3072U
#define WEB_FILE_HTTPD_STOP_TASK_PRIORITY         4U

static const char *TAG = "web_file_service";

/**
 * @brief 独占 HTTPD 句柄并执行一次同步销毁
 *
 * ESP-IDF 的 `httpd_stop()` 内部会无期限轮询 HTTPD Task 终态，因此必须与有界等待的公共
 * `stop()` 调用隔离。SDK 调用返回后先发出固定完成信号，再把结果作为对 Service 上下文的
 * 最后一次写入；结果可见后 Task 只会挂起，不再访问 Service 同步对象或状态。生命周期调用
 * 观察到结果后负责显式删除本 Task，所以超时路径不会强杀仍持有 HTTPD 句柄的执行者。
 *
 * @param[in,out] argument 指向静态 Service 上下文，且 `server` 已由本 Task 独占
 */
static void web_file_httpd_stop_task(void *argument)
{
    web_file_service_context_t *context = (web_file_service_context_t *) argument;

    xSemaphoreTake(context->lock, portMAX_DELAY);
    const httpd_handle_t    server     = context->server;
    const SemaphoreHandle_t completion = context->httpd_stop_completed;
    xSemaphoreGive(context->lock);

    const esp_err_t result = httpd_stop(server);

    (void) xSemaphoreGive(completion);

    xSemaphoreTake(context->lock, portMAX_DELAY);
    if (result == ESP_OK)
    {
        context->server = NULL;
    }
    context->httpd_stop_result       = result;
    context->httpd_stop_result_ready = true;
    context->httpd_stop_in_progress  = false;
    xSemaphoreGive(context->lock);

    ESP_LOGD(TAG, "HTTPD 清理 Task 已发布结果，等待生命周期调用回收");
    for (;;)
    {
        vTaskSuspend(NULL);
    }
}

esp_err_t web_file_httpd_stop_task_create(web_file_service_context_t *context, TaskHandle_t *out_task)
{
    if (context == NULL || out_task == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_task                = NULL;
    const BaseType_t created = xTaskCreate(web_file_httpd_stop_task,
                                           "web_file_stop",
                                           WEB_FILE_HTTPD_STOP_TASK_STACK_SIZE_BYTES,
                                           context,
                                           WEB_FILE_HTTPD_STOP_TASK_PRIORITY,
                                           out_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
