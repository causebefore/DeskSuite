/**
 * @file remote_log_internal.h
 * @brief remote_log 组件内部共享状态与事件定义
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_upload.h"
#include "remote_log.h"

#define REMOTE_LOG_QUEUE_CAPACITY_MAX 256U

_Static_assert(REMOTE_LOG_SESSION_ID_MAX == LOG_UPLOAD_SESSION_ID_MAX,
               "remote_log 与 log_upload 的 session_id 容量必须一致");

/** @brief 上传 Task 队列事件类型 */
typedef enum
{
    REMOTE_LOG_EVENT_LINE = 0,
    REMOTE_LOG_EVENT_STOP,
} remote_log_event_type_t;

/** @brief 上传 Task 队列元素；停止事件使用额外保留的物理槽位 */
typedef struct
{
    remote_log_event_type_t type;
    log_upload_line_t       line;
} remote_log_event_t;

/**
 * @brief remote_log 唯一运行时
 *
 * Task 是可变上传状态的唯一写入者；公共生命周期函数与 Log V2
 * 包装入口只在临界区内更新 共享字段。
 */
typedef struct
{
    bool                       initialized;
    bool                       configured;
    bool                       capture_enabled;
    bool                       stop_requested;
    remote_log_state_t         state;
    remote_log_config_t        config;
    protocol_backend_context_t backend;
    char                       session_id[REMOTE_LOG_SESSION_ID_MAX];

    QueueHandle_t      events;
    SemaphoreHandle_t  log_slots;
    SemaphoreHandle_t  task_stopped;
    TaskHandle_t       task;
    log_upload_line_t *batch;

    uint32_t  captured_lines;
    uint32_t  queued_lines;
    uint32_t  uploaded_lines;
    uint32_t  dropped_lines;
    uint32_t  upload_failures;
    uint32_t  next_sequence;
    uint32_t  active_captures;
    esp_err_t last_error;
} remote_log_runtime_t;

extern remote_log_runtime_t g_remote_log_runtime;
extern portMUX_TYPE         g_remote_log_lock;

/**
 * @brief 包装 ESP-IDF Log V2 日志入口并保留原串口输出
 *
 * 链接器通过 --wrap=esp_log 把普通 ESP_LOGx 调用重定向到本函数。函数先调用
 * esp_log_va() 完成原始输出，再把同一条日志的结构化字段交给远端日志缓存。
 *
 * @param[in] config ESP-IDF Log V2 日志配置
 * @param[in] tag 日志标签，可为空
 * @param[in] format 正文格式串
 */
void __wrap_esp_log(esp_log_config_t config, const char *tag, const char *format, ...);
