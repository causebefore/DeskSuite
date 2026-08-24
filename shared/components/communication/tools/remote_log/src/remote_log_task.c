#include "remote_log_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "network_manager.h"

#define REMOTE_LOG_QUEUE_CAPACITY_DEFAULT    64U
#define REMOTE_LOG_BATCH_CAPACITY_DEFAULT    8U
#define REMOTE_LOG_BATCH_WAIT_MS_DEFAULT     100U
#define REMOTE_LOG_RETRY_INTERVAL_MS_DEFAULT 3000U
#define REMOTE_LOG_HTTP_TIMEOUT_MS_DEFAULT   3000
#define REMOTE_LOG_TASK_STACK_SIZE_DEFAULT   6144U
#define REMOTE_LOG_TASK_PRIORITY_DEFAULT     3U

/** @brief 连续上传失败的技术退避倍率，最后一档封顶 */
static const uint8_t s_retry_backoff_multipliers[] = { 1U, 5U, 15U, 60U };

/** @brief 日志标签 */
static const char *TAG = "remote_log_task";

remote_log_runtime_t g_remote_log_runtime;
portMUX_TYPE         g_remote_log_lock = portMUX_INITIALIZER_UNLOCKED;

static bool remote_log_config_valid(const remote_log_config_t *config)
{
    return config != NULL && config->queue_capacity > 0U && config->queue_capacity <= REMOTE_LOG_QUEUE_CAPACITY_MAX
           && config->batch_capacity > 0U && config->batch_capacity <= config->queue_capacity
           && config->retry_interval_ms > 0U && config->http_timeout_ms > 0 && config->task_stack_size_bytes > 0U
           && config->task_priority < (uint32_t) configMAX_PRIORITIES;
}

static bool remote_log_stop_requested(void)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool requested = g_remote_log_runtime.stop_requested;
    taskEXIT_CRITICAL(&g_remote_log_lock);
    return requested;
}

/**
 * @brief 更新上传状态，同时保持停止流程的单向收敛
 *
 * stop 请求可能在同步协议调用期间到达；调用返回后不得用 IDLE 等状态覆盖
 * STOPPING。
 *
 * @param[in] state 目标上传状态
 * @param[in] last_error 最近错误
 */
static void remote_log_set_state(remote_log_state_t state, esp_err_t last_error)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    g_remote_log_runtime.state      = g_remote_log_runtime.stop_requested ? REMOTE_LOG_STATE_STOPPING : state;
    g_remote_log_runtime.last_error = last_error;
    taskEXIT_CRITICAL(&g_remote_log_lock);
}

static void remote_log_record_upload_failure(esp_err_t error)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    ++g_remote_log_runtime.upload_failures;
    g_remote_log_runtime.last_error = error;
    taskEXIT_CRITICAL(&g_remote_log_lock);
}

static void remote_log_record_dropped_batch(size_t count)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    g_remote_log_runtime.dropped_lines += (uint32_t) count;
    taskEXIT_CRITICAL(&g_remote_log_lock);
}

static TickType_t remote_log_ms_to_ticks(uint32_t duration_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(duration_ms);
    return duration_ms > 0U && ticks == 0U ? 1U : ticks;
}

/**
 * @brief 按连续失败阶段计算上传重试等待时间
 *
 * 配置值是第一档等待时间；乘法在 uint32_t 范围内饱和，避免异常配置导致回绕后高频重试。
 *
 * @param[in] retry_stage 当前连续失败阶段
 * @return 本轮等待毫秒数
 */
static uint32_t remote_log_retry_delay_ms(uint8_t retry_stage)
{
    const size_t stage_count = sizeof(s_retry_backoff_multipliers) / sizeof(s_retry_backoff_multipliers[0]);
    const size_t stage_index = retry_stage < stage_count ? retry_stage : stage_count - 1U;
    const uint32_t multiplier = s_retry_backoff_multipliers[stage_index];
    const uint32_t base_delay = g_remote_log_runtime.config.retry_interval_ms;
    return base_delay > UINT32_MAX / multiplier ? UINT32_MAX : base_delay * multiplier;
}

/**
 * @brief 推进连续失败阶段并在最后一档封顶
 *
 * @param[in] retry_stage 当前连续失败阶段
 * @return 下一次失败应使用的阶段
 */
static uint8_t remote_log_next_retry_stage(uint8_t retry_stage)
{
    const uint8_t last_stage =
        (uint8_t) (sizeof(s_retry_backoff_multipliers) / sizeof(s_retry_backoff_multipliers[0]) - 1U);
    return retry_stage < last_stage ? (uint8_t) (retry_stage + 1U) : last_stage;
}

static void remote_log_wait_retry(uint8_t retry_stage)
{
    (void) ulTaskNotifyTake(pdTRUE, remote_log_ms_to_ticks(remote_log_retry_delay_ms(retry_stage)));
}

static const char *remote_log_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason)
    {
        case ESP_RST_POWERON:
            return "POWERON_RESET";
        case ESP_RST_EXT:
            return "EXT_RESET";
        case ESP_RST_SW:
            return "SW_RESET";
        case ESP_RST_PANIC:
            return "PANIC_RESET";
        case ESP_RST_INT_WDT:
            return "INT_WDT_RESET";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT_RESET";
        case ESP_RST_WDT:
            return "WDT_RESET";
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP_RESET";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT_RESET";
        case ESP_RST_SDIO:
            return "SDIO_RESET";
        case ESP_RST_UNKNOWN:
        default:
            return "UNKNOWN_RESET";
    }
}

/**
 * @brief 等待 Network Manager 报告 ONLINE
 *
 * 本函数不控制网络生命周期。离线期间只记录 Network Manager
 * 的错误事实并按配置退避；停止 通知会提前唤醒退避等待。
 *
 * @return true 已在线；false 收到停止请求
 */
static bool remote_log_wait_online(void)
{
    while (!remote_log_stop_requested())
    {
        network_manager_status_t status = { 0 };
        const esp_err_t          error  = network_manager_get_status_copy(&status);
        if (error == ESP_OK && status.state == NETWORK_STATE_ONLINE)
        {
            return true;
        }

        const esp_err_t state_error = error != ESP_OK ? error : status.last_error;
        remote_log_set_state(REMOTE_LOG_STATE_WAITING_NETWORK, state_error);
        remote_log_wait_retry(0U);
    }
    return false;
}

/**
 * @brief 建立本次设备启动对应的服务端日志会话
 *
 * session_id 在 remote_log 生命周期中复用；上传 Task 停止后再次启动不会重复发送
 * boot，只有 configure_copy 修改服务端身份时才会清空会话。
 *
 * @return true 会话已可用；false 收到停止请求
 */
static bool remote_log_ensure_session(void)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool has_session = g_remote_log_runtime.session_id[0] != '\0';
    taskEXIT_CRITICAL(&g_remote_log_lock);
    if (has_session)
    {
        remote_log_set_state(REMOTE_LOG_STATE_IDLE, ESP_OK);
        return true;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t               retry_stage = 0U;

    while (!remote_log_stop_requested())
    {
        if (!remote_log_wait_online())
        {
            return false;
        }

        network_manager_diagnostics_t diagnostics       = { 0 };
        const esp_err_t               diagnostics_error = network_manager_get_diagnostics_copy(&diagnostics);
        const log_upload_boot_t       boot              = {
            .firmware_version = app != NULL ? app->version : "unknown",
            .reset_reason     = remote_log_reset_reason_name(esp_reset_reason()),
            .ip = diagnostics_error == ESP_OK && diagnostics.link_snapshot_error == ESP_OK ? diagnostics.link.ip : "",
        };
        remote_log_set_state(REMOTE_LOG_STATE_STARTING_SESSION, ESP_OK);
        char            session_id[REMOTE_LOG_SESSION_ID_MAX] = { 0 };
        const esp_err_t error = log_upload_start(&g_remote_log_runtime.backend,
                                                 g_remote_log_runtime.config.http_timeout_ms,
                                                 &boot,
                                                 session_id,
                                                 sizeof(session_id));
        if (error == ESP_OK)
        {
            taskENTER_CRITICAL(&g_remote_log_lock);
            memcpy(g_remote_log_runtime.session_id, session_id, sizeof(session_id));
            g_remote_log_runtime.last_error = ESP_OK;
            taskEXIT_CRITICAL(&g_remote_log_lock);
            remote_log_set_state(REMOTE_LOG_STATE_IDLE, ESP_OK);
            return true;
        }

        remote_log_record_upload_failure(error);
        remote_log_wait_retry(retry_stage);
        retry_stage = remote_log_next_retry_stage(retry_stage);
    }
    return false;
}

static bool remote_log_receive_event(remote_log_event_t *out_event, TickType_t ticks_to_wait)
{
    if (xQueueReceive(g_remote_log_runtime.events, out_event, ticks_to_wait) != pdTRUE)
    {
        return false;
    }
    if (out_event->type == REMOTE_LOG_EVENT_LINE)
    {
        (void) xSemaphoreGive(g_remote_log_runtime.log_slots);
        taskENTER_CRITICAL(&g_remote_log_lock);
        if (g_remote_log_runtime.queued_lines > 0U)
        {
            --g_remote_log_runtime.queued_lines;
        }
        taskEXIT_CRITICAL(&g_remote_log_lock);
    }
    return true;
}

static void remote_log_discard_pending_stop(void)
{
    remote_log_event_t event;
    if (xQueuePeek(g_remote_log_runtime.events, &event, 0U) == pdTRUE && event.type == REMOTE_LOG_EVENT_STOP)
    {
        (void) xQueueReceive(g_remote_log_runtime.events, &event, 0U);
    }
}

/**
 * @brief 取得一个有界日志批次
 *
 * 第一条日志使用无限等待；后续日志只在 batch_wait_ms
 * 内聚合。若收到停止事件，已经从队列
 * 取出的批次由调用方计为丢弃，队列中其他日志继续保留。
 *
 * @param[out] out_count 批次条数
 * @return true 已取得可上传批次；false 收到停止请求
 */
static bool remote_log_collect_batch(size_t *out_count)
{
    *out_count = 0U;
    remote_log_event_t event;
    if (!remote_log_receive_event(&event, portMAX_DELAY) || event.type == REMOTE_LOG_EVENT_STOP)
    {
        return false;
    }

    g_remote_log_runtime.batch[(*out_count)++] = event.line;
    while (*out_count < g_remote_log_runtime.config.batch_capacity)
    {
        if (!remote_log_receive_event(&event, remote_log_ms_to_ticks(g_remote_log_runtime.config.batch_wait_ms)))
        {
            break;
        }
        if (event.type == REMOTE_LOG_EVENT_STOP)
        {
            return false;
        }
        g_remote_log_runtime.batch[(*out_count)++] = event.line;
    }
    return !remote_log_stop_requested();
}

/**
 * @brief 上传当前批次并在失败时原地重试
 *
 * 批次数组在整个重试期间由上传 Task
 * 独占，不重新入队，因此不会改变日志顺序。停止请求会
 * 终止后续重试，由调用方把该批次计为丢弃。
 *
 * @param[in] count 批次条数
 * @return true 服务端已接受；false 收到停止请求
 */
static bool remote_log_upload_batch(size_t count)
{
    uint8_t retry_stage = 0U;
    while (!remote_log_stop_requested())
    {
        if (!remote_log_wait_online())
        {
            return false;
        }

        remote_log_set_state(REMOTE_LOG_STATE_UPLOADING, ESP_OK);
        char            next_session_id[REMOTE_LOG_SESSION_ID_MAX] = { 0 };
        const esp_err_t error = log_upload_batch(&g_remote_log_runtime.backend,
                                                 g_remote_log_runtime.config.http_timeout_ms,
                                                 g_remote_log_runtime.session_id,
                                                 g_remote_log_runtime.batch,
                                                 count,
                                                 next_session_id,
                                                 sizeof(next_session_id));
        if (error == ESP_OK)
        {
            taskENTER_CRITICAL(&g_remote_log_lock);
            g_remote_log_runtime.uploaded_lines += (uint32_t) count;
            g_remote_log_runtime.last_error = ESP_OK;
            if (next_session_id[0] != '\0')
            {
                memcpy(g_remote_log_runtime.session_id, next_session_id, sizeof(next_session_id));
            }
            taskEXIT_CRITICAL(&g_remote_log_lock);
            remote_log_set_state(REMOTE_LOG_STATE_IDLE, ESP_OK);
            return true;
        }

        remote_log_record_upload_failure(error);
        remote_log_wait_retry(retry_stage);
        retry_stage = remote_log_next_retry_stage(retry_stage);
    }
    return false;
}

/**
 * @brief 远端日志上传 Task
 *
 * Task
 * 串行拥有会话创建、批次组装和上传重试。收到停止请求后释放当前批次的逻辑所有权，
 * 通知 stop 调用者并挂起，由 stop 统一删除 Task 句柄。
 *
 * @param[in] context 未使用
 */
static void remote_log_task(void *context)
{
    (void) context;
    taskENTER_CRITICAL(&g_remote_log_lock);
    g_remote_log_runtime.task = xTaskGetCurrentTaskHandle();
    taskEXIT_CRITICAL(&g_remote_log_lock);

    if (remote_log_ensure_session())
    {
        while (!remote_log_stop_requested())
        {
            remote_log_set_state(REMOTE_LOG_STATE_IDLE, ESP_OK);
            size_t batch_count = 0U;
            if (!remote_log_collect_batch(&batch_count))
            {
                remote_log_record_dropped_batch(batch_count);
                break;
            }
            if (!remote_log_upload_batch(batch_count))
            {
                remote_log_record_dropped_batch(batch_count);
                break;
            }
        }
    }

    remote_log_discard_pending_stop();
    (void) xSemaphoreGive(g_remote_log_runtime.task_stopped);
    vTaskSuspend(NULL);
}

esp_err_t remote_log_config_set_defaults(remote_log_config_t *out_config)
{
    if (out_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_config = (remote_log_config_t) {
        .queue_capacity        = REMOTE_LOG_QUEUE_CAPACITY_DEFAULT,
        .batch_capacity        = REMOTE_LOG_BATCH_CAPACITY_DEFAULT,
        .batch_wait_ms         = REMOTE_LOG_BATCH_WAIT_MS_DEFAULT,
        .retry_interval_ms     = REMOTE_LOG_RETRY_INTERVAL_MS_DEFAULT,
        .http_timeout_ms       = REMOTE_LOG_HTTP_TIMEOUT_MS_DEFAULT,
        .task_stack_size_bytes = REMOTE_LOG_TASK_STACK_SIZE_DEFAULT,
        .task_priority         = REMOTE_LOG_TASK_PRIORITY_DEFAULT,
    };
    return ESP_OK;
}

esp_err_t remote_log_init(const remote_log_config_t *config)
{
    if (!remote_log_config_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool already_initialized = g_remote_log_runtime.initialized;
    taskEXIT_CRITICAL(&g_remote_log_lock);
    if (already_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    log_upload_line_t *batch = calloc(config->batch_capacity, sizeof(*batch));
    if (batch == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    QueueHandle_t events = xQueueCreate((UBaseType_t) (config->queue_capacity + 1U), sizeof(remote_log_event_t));
    if (events == NULL)
    {
        free(batch);
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t log_slots =
        xSemaphoreCreateCounting((UBaseType_t) config->queue_capacity, (UBaseType_t) config->queue_capacity);
    if (log_slots == NULL)
    {
        vQueueDelete(events);
        free(batch);
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t task_stopped = xSemaphoreCreateBinary();
    if (task_stopped == NULL)
    {
        vSemaphoreDelete(log_slots);
        vQueueDelete(events);
        free(batch);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&g_remote_log_lock);
    memset(&g_remote_log_runtime, 0, sizeof(g_remote_log_runtime));
    g_remote_log_runtime.initialized     = true;
    g_remote_log_runtime.capture_enabled = true;
    g_remote_log_runtime.state           = REMOTE_LOG_STATE_STOPPED;
    g_remote_log_runtime.config          = *config;
    g_remote_log_runtime.events          = events;
    g_remote_log_runtime.log_slots       = log_slots;
    g_remote_log_runtime.task_stopped    = task_stopped;
    g_remote_log_runtime.batch           = batch;
    g_remote_log_runtime.next_sequence   = 1U;
    g_remote_log_runtime.last_error      = ESP_OK;
    taskEXIT_CRITICAL(&g_remote_log_lock);

    return ESP_OK;
}

esp_err_t remote_log_configure_copy(const protocol_backend_context_t *backend)
{
    if (!protocol_backend_context_is_valid(backend))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const protocol_backend_context_t backend_copy = *backend;

    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool can_configure = g_remote_log_runtime.initialized
                               && g_remote_log_runtime.state == REMOTE_LOG_STATE_STOPPED
                               && g_remote_log_runtime.task == NULL;
    if (can_configure)
    {
        g_remote_log_runtime.backend       = backend_copy;
        g_remote_log_runtime.session_id[0] = '\0';
        g_remote_log_runtime.configured    = true;
        g_remote_log_runtime.last_error    = ESP_OK;
    }
    taskEXIT_CRITICAL(&g_remote_log_lock);
    return can_configure ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t remote_log_start(void)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool can_start = g_remote_log_runtime.initialized && g_remote_log_runtime.configured
                           && g_remote_log_runtime.state == REMOTE_LOG_STATE_STOPPED
                           && g_remote_log_runtime.task == NULL;
    if (can_start)
    {
        g_remote_log_runtime.stop_requested = false;
        g_remote_log_runtime.state          = REMOTE_LOG_STATE_WAITING_NETWORK;
        g_remote_log_runtime.last_error     = ESP_OK;
    }
    const uint32_t stack_size = g_remote_log_runtime.config.task_stack_size_bytes;
    const uint32_t priority   = g_remote_log_runtime.config.task_priority;
    taskEXIT_CRITICAL(&g_remote_log_lock);
    if (!can_start)
    {
        return ESP_ERR_INVALID_STATE;
    }

    (void) xSemaphoreTake(g_remote_log_runtime.task_stopped, 0U);
    TaskHandle_t task = NULL;
    if (xTaskCreate(remote_log_task,
                    "remote_log",
                    (configSTACK_DEPTH_TYPE) stack_size,
                    NULL,
                    (UBaseType_t) priority,
                    &task)
        != pdPASS)
    {
        taskENTER_CRITICAL(&g_remote_log_lock);
        g_remote_log_runtime.state = REMOTE_LOG_STATE_STOPPED;
        taskEXIT_CRITICAL(&g_remote_log_lock);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&g_remote_log_lock);
    g_remote_log_runtime.task = task;
    taskEXIT_CRITICAL(&g_remote_log_lock);
    return ESP_OK;
}

esp_err_t remote_log_stop(uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool already_stopping = g_remote_log_runtime.initialized
                                  && g_remote_log_runtime.state == REMOTE_LOG_STATE_STOPPING
                                  && g_remote_log_runtime.task != NULL;
    const bool can_request = g_remote_log_runtime.initialized && g_remote_log_runtime.state != REMOTE_LOG_STATE_STOPPED
                             && g_remote_log_runtime.task != NULL;
    TaskHandle_t      task = g_remote_log_runtime.task;
    SemaphoreHandle_t task_stopped = g_remote_log_runtime.task_stopped;
    if (can_request && !already_stopping)
    {
        g_remote_log_runtime.stop_requested = true;
        g_remote_log_runtime.state          = REMOTE_LOG_STATE_STOPPING;
    }
    taskEXIT_CRITICAL(&g_remote_log_lock);
    if (!can_request)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!already_stopping)
    {
        const remote_log_event_t stop_event = {
            .type = REMOTE_LOG_EVENT_STOP,
        };
        if (xQueueSendToFront(g_remote_log_runtime.events, &stop_event, 0U) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }
        xTaskNotifyGive(task);
    }

    if (xSemaphoreTake(task_stopped, remote_log_ms_to_ticks(timeout_ms)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    vTaskDelete(task);
    taskENTER_CRITICAL(&g_remote_log_lock);
    if (g_remote_log_runtime.task == task)
    {
        g_remote_log_runtime.task           = NULL;
        g_remote_log_runtime.stop_requested = false;
        g_remote_log_runtime.state          = REMOTE_LOG_STATE_STOPPED;
    }
    taskEXIT_CRITICAL(&g_remote_log_lock);
    return ESP_OK;
}

esp_err_t remote_log_deinit(void)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool can_deinit = g_remote_log_runtime.initialized && g_remote_log_runtime.state == REMOTE_LOG_STATE_STOPPED
                            && g_remote_log_runtime.task == NULL;
    if (can_deinit)
    {
        g_remote_log_runtime.capture_enabled = false;
    }
    QueueHandle_t      events       = g_remote_log_runtime.events;
    SemaphoreHandle_t  log_slots    = g_remote_log_runtime.log_slots;
    SemaphoreHandle_t  task_stopped = g_remote_log_runtime.task_stopped;
    log_upload_line_t *batch        = g_remote_log_runtime.batch;
    taskEXIT_CRITICAL(&g_remote_log_lock);
    if (!can_deinit)
    {
        return ESP_ERR_INVALID_STATE;
    }

    while (true)
    {
        taskENTER_CRITICAL(&g_remote_log_lock);
        const bool capture_idle = g_remote_log_runtime.active_captures == 0U;
        taskEXIT_CRITICAL(&g_remote_log_lock);
        if (capture_idle)
        {
            break;
        }
        vTaskDelay(1U);
    }

    taskENTER_CRITICAL(&g_remote_log_lock);
    memset(&g_remote_log_runtime, 0, sizeof(g_remote_log_runtime));
    taskEXIT_CRITICAL(&g_remote_log_lock);

    vSemaphoreDelete(task_stopped);
    vSemaphoreDelete(log_slots);
    vQueueDelete(events);
    free(batch);
    return ESP_OK;
}

esp_err_t remote_log_get_status_copy(remote_log_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool initialized = g_remote_log_runtime.initialized;
    if (initialized)
    {
        out_status->state           = g_remote_log_runtime.state;
        out_status->captured_lines  = g_remote_log_runtime.captured_lines;
        out_status->queued_lines    = g_remote_log_runtime.queued_lines;
        out_status->uploaded_lines  = g_remote_log_runtime.uploaded_lines;
        out_status->dropped_lines   = g_remote_log_runtime.dropped_lines;
        out_status->upload_failures = g_remote_log_runtime.upload_failures;
        out_status->last_error      = g_remote_log_runtime.last_error;
        memcpy(out_status->session_id, g_remote_log_runtime.session_id, sizeof(out_status->session_id));
    }
    taskEXIT_CRITICAL(&g_remote_log_lock);
    return initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}
