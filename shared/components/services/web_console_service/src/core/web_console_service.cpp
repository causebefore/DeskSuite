/**
 * @file web_console_service.cpp
 * @brief 网页控制台 Service 生命周期与资源所有权实现
 */
#include "web_console_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "web_console_service_internal.hpp"
#if CONFIG_WEB_CONSOLE_FILES
#include "web_console_files_internal.hpp"
#endif

#define WEB_FILE_HTTPD_MAX_OPEN_SOCKETS    1U
#define WEB_FILE_HTTPD_MAX_URI_HANDLERS    WEB_CONSOLE_ROUTE_COUNT
#define WEB_FILE_HTTPD_IO_TIMEOUT_S        5U
#define WEB_FILE_ACCESS_CODE_SPACE         1000000U
#define WEB_FILE_START_ROLLBACK_TIMEOUT_MS 6000U

static_assert(WEB_FILE_HTTPD_MAX_URI_HANDLERS == WEB_CONSOLE_ROUTE_COUNT,
              "HTTPD handler 上限必须覆盖全部 Console Core 与 Files 路由");

static const char *TAG = "web_console_service";

/** @brief 构造尚未初始化且不持有活动 socket 的进程期初始上下文 */
static web_console_service_context_t web_file_make_initial_context(void)
{
    web_console_service_context_t context{};
    context.state = WEB_CONSOLE_SERVICE_STATE_UNINITIALIZED;
    return context;
}

web_console_service_context_t s_context = web_file_make_initial_context();

static void       web_file_trigger_client_close(httpd_handle_t server);
static esp_err_t  web_file_wait_for_handlers(int64_t deadline_us);
static TickType_t web_file_remaining_wait_ticks(int64_t deadline_us);
static esp_err_t  web_file_start_httpd_stop_task(void);
static esp_err_t  web_file_wait_for_httpd_stop_result(int64_t deadline_us);
static esp_err_t  web_file_collect_httpd_stop_result(esp_err_t *out_result);

/**
 * @brief 以不会被编译器省略的逐字节写入清除栈上秘密
 *
 * @param[in,out] data 待清除内存
 * @param[in] size 待清除字节数
 */
static void web_file_secure_clear(void *data, size_t size)
{
    volatile uint8_t *cursor = static_cast<volatile uint8_t *>(data);
    while (size > 0U)
    {
        *cursor = 0U;
        ++cursor;
        --size;
    }
}

/**
 * @brief 完成没有 HTTPD 句柄的启动失败回滚
 *
 * 本函数在锁内清空认证秘密、退出生命周期占用并回到 `INITIALIZED`；调用前不得已经向
 * `s_context.server` 发布 HTTPD 句柄。
 *
 * @param[in] error 启动失败原因
 * @return 原始失败原因
 */
static esp_err_t web_file_finish_start_without_server(esp_err_t error)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    web_file_auth_reset(&s_context.auth, NULL);
    s_context.state                   = WEB_CONSOLE_SERVICE_STATE_INITIALIZED;
    s_context.accepting_requests      = false;
    s_context.lifecycle_active        = false;
    s_context.registered_route_count = 0U;
    s_context.ingress_close_error     = ESP_OK;
    s_context.ingress_close_queued    = false;
    s_context.httpd_stop_task         = NULL;
    s_context.httpd_stop_result       = ESP_OK;
    s_context.httpd_stop_in_progress  = false;
    s_context.httpd_stop_result_ready = false;
    s_context.last_error              = error;
    xSemaphoreGive(s_context.lock);
    return error;
}

/**
 * @brief 停止部分启动的 HTTPD 并收敛启动失败状态
 *
 * 本函数使用固定六秒绝对期限触发客户端关闭、在 HTTPD Task 注销已注册 URI，并等待活动
 * handler 排空，再等待一次性清理 Task 完成合法 `httpd_stop()`。所有等待共享同一期限；
 * 超时时不强杀仍持有 HTTPD 的 Task，也不释放其同步资源。无论清理结果如何都会清空认证秘密
 * 并退出生命周期占用。清理成功时回收 Task、释放服务器所有权、回到 `INITIALIZED` 并保留
 * 原始启动错误；清理失败时保留 Task、服务器句柄或未收敛入口，进入 `CLEANUP_FAILED` 拒绝
 * `start()` 并由后续 `stop()` 重试。
 *
 * @param[in] server 部分启动的 HTTPD 句柄
 * @param[in] start_error 原始启动错误
 * @return 原始启动错误，或更优先的 HTTPD 清理错误
 */
static esp_err_t web_file_rollback_started_httpd(httpd_handle_t server, esp_err_t start_error)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t) WEB_FILE_START_ROLLBACK_TIMEOUT_MS * 1000LL;

    web_file_trigger_client_close(server);
    esp_err_t cleanup_error = web_console_http_close_ingress(server, deadline_us);
    if (cleanup_error == ESP_OK)
    {
        cleanup_error = web_file_wait_for_handlers(deadline_us);
    }
    if (cleanup_error == ESP_OK)
    {
#if CONFIG_WEB_CONSOLE_FILES
        web_console_files_cleanup_after_handlers();
#endif
        cleanup_error =
            web_file_remaining_wait_ticks(deadline_us) == 0U ? ESP_ERR_TIMEOUT : web_file_start_httpd_stop_task();
    }
    if (cleanup_error == ESP_OK)
    {
        cleanup_error = web_file_wait_for_httpd_stop_result(deadline_us);
    }
    if (cleanup_error == ESP_OK)
    {
        esp_err_t stop_result;
        cleanup_error = web_file_collect_httpd_stop_result(&stop_result);
        if (cleanup_error == ESP_OK)
        {
            cleanup_error = stop_result;
        }
    }

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    web_file_auth_reset(&s_context.auth, NULL);
    s_context.accepting_requests = false;
    s_context.lifecycle_active   = false;
    if (cleanup_error == ESP_OK)
    {
        s_context.state      = WEB_CONSOLE_SERVICE_STATE_INITIALIZED;
        s_context.last_error = start_error;
    }
    else
    {
        s_context.state      = WEB_CONSOLE_SERVICE_STATE_CLEANUP_FAILED;
        s_context.last_error = cleanup_error;
    }
    xSemaphoreGive(s_context.lock);
    return cleanup_error == ESP_OK ? start_error : cleanup_error;
}

esp_err_t web_console_service_init_borrow(const web_console_service_config_t *config)
{
    if (config == NULL || config->server_port == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_WEB_CONSOLE_FILES
    if (config->files == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
#else
    if (config->files != NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
#endif

    if (s_context.state != WEB_CONSOLE_SERVICE_STATE_UNINITIALIZED || s_context.lock != NULL
        || s_context.handlers_drained != NULL || s_context.ingress_closed != NULL
        || s_context.httpd_stop_completed != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t handlers_drained = xSemaphoreCreateBinary();
    if (handlers_drained == NULL)
    {
        vSemaphoreDelete(lock);
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t ingress_closed = xSemaphoreCreateBinary();
    if (ingress_closed == NULL)
    {
        vSemaphoreDelete(handlers_drained);
        vSemaphoreDelete(lock);
        return ESP_ERR_NO_MEM;
    }
    SemaphoreHandle_t httpd_stop_completed = xSemaphoreCreateBinary();
    if (httpd_stop_completed == NULL)
    {
        vSemaphoreDelete(ingress_closed);
        vSemaphoreDelete(handlers_drained);
        vSemaphoreDelete(lock);
        return ESP_ERR_NO_MEM;
    }

    memset(&s_context, 0, sizeof(s_context));
    s_context.lock                   = lock;
    s_context.handlers_drained       = handlers_drained;
    s_context.ingress_closed         = ingress_closed;
    s_context.httpd_stop_completed   = httpd_stop_completed;
    s_context.state                  = WEB_CONSOLE_SERVICE_STATE_INITIALIZED;
    s_context.server_port            = config->server_port;
    s_context.last_error             = ESP_OK;
    s_context.ingress_close_error    = ESP_OK;
#if CONFIG_WEB_CONSOLE_FILES
    web_console_files_reset_context();
    const esp_err_t configure_error = web_console_files_configure_borrow(config->files);
    if (configure_error != ESP_OK)
    {
        web_console_files_reset_context();
        memset(&s_context, 0, sizeof(s_context));
        s_context.state = WEB_CONSOLE_SERVICE_STATE_UNINITIALIZED;
        vSemaphoreDelete(httpd_stop_completed);
        vSemaphoreDelete(ingress_closed);
        vSemaphoreDelete(handlers_drained);
        vSemaphoreDelete(lock);
        return configure_error;
    }
#endif
    return ESP_OK;
}

esp_err_t web_console_service_start(void)
{
    SemaphoreHandle_t lock = s_context.lock;
    if (lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_context.state != WEB_CONSOLE_SERVICE_STATE_INITIALIZED || s_context.lifecycle_active || s_context.server != NULL
        || s_context.active_handlers != 0U
#if CONFIG_WEB_CONSOLE_FILES
        || !web_console_files_is_idle_locked()
#endif
        || !web_console_http_routes_are_released_locked() || s_context.ingress_close_queued
        || s_context.httpd_stop_task != NULL || s_context.httpd_stop_in_progress || s_context.httpd_stop_result_ready)
    {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_context.state                  = WEB_CONSOLE_SERVICE_STATE_STARTING;
    s_context.accepting_requests     = false;
    s_context.lifecycle_active       = true;
    s_context.last_error             = ESP_OK;
    s_context.ingress_close_error    = ESP_OK;
    xSemaphoreGive(lock);

#if CONFIG_WEB_CONSOLE_FILES
    const esp_err_t recovery_error = web_console_files_prepare_start();
    if (recovery_error != ESP_OK)
    {
        ESP_LOGE(TAG, "恢复残留上传事务失败，拒绝启动网页文件服务: %s", esp_err_to_name(recovery_error));
        return web_file_finish_start_without_server(recovery_error);
    }
#endif

    uint32_t random_value;
    char     access_code[WEB_FILE_ACCESS_CODE_BUFFER_SIZE]{};
    esp_fill_random(&random_value, sizeof(random_value));
    const int code_size =
        snprintf(access_code, sizeof(access_code), "%06" PRIu32, random_value % WEB_FILE_ACCESS_CODE_SPACE);
    if (code_size != (int) WEB_FILE_ACCESS_CODE_LENGTH)
    {
        web_file_secure_clear(access_code, sizeof(access_code));
        return web_file_finish_start_without_server(ESP_FAIL);
    }

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.server_port       = s_context.server_port;
    config.max_open_sockets  = WEB_FILE_HTTPD_MAX_OPEN_SOCKETS;
    config.max_uri_handlers  = WEB_FILE_HTTPD_MAX_URI_HANDLERS;
    config.lru_purge_enable  = false;
    config.recv_wait_timeout = WEB_FILE_HTTPD_IO_TIMEOUT_S;
    config.send_wait_timeout = WEB_FILE_HTTPD_IO_TIMEOUT_S;
    config.uri_match_fn      = NULL;

    httpd_handle_t server    = NULL;
    esp_err_t      error     = httpd_start(&server, &config);
    if (error != ESP_OK)
    {
        web_file_secure_clear(access_code, sizeof(access_code));
        return web_file_finish_start_without_server(error);
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    s_context.server = server;
    xSemaphoreGive(lock);

    error = web_console_http_register_handlers(server);
    if (error != ESP_OK)
    {
        web_file_secure_clear(access_code, sizeof(access_code));
        return web_file_rollback_started_httpd(server, error);
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    web_file_auth_reset(&s_context.auth, access_code);
    s_context.state              = WEB_CONSOLE_SERVICE_STATE_RUNNING;
    s_context.accepting_requests = true;
    s_context.lifecycle_active   = false;
    s_context.last_error         = ESP_OK;
    xSemaphoreGive(lock);
    web_file_secure_clear(access_code, sizeof(access_code));
    return ESP_OK;
}

/**
 * @brief 尽力触发关闭当前 HTTPD 的全部客户端 socket
 *
 * 调用方借用仍由 Service 持有的 `server` 句柄，且不得持有 Service 状态锁。本函数只发起
 * 关闭、不等待 handler 退出；枚举或触发失败仅记录错误，后续 handler 计数排空仍是停止流程的
 * 完成判据。
 *
 * @param[in] server Service 当前持有的 HTTPD 句柄
 */
static void web_file_trigger_client_close(httpd_handle_t server)
{
    int             client_fds[WEB_FILE_HTTPD_MAX_OPEN_SOCKETS];
    size_t          client_count = WEB_FILE_HTTPD_MAX_OPEN_SOCKETS;
    const esp_err_t list_error   = httpd_get_client_list(server, &client_count, client_fds);
    if (list_error != ESP_OK)
    {
        ESP_LOGW(TAG, "枚举 HTTP 客户端失败，将继续尝试停止服务器: %s", esp_err_to_name(list_error));
        return;
    }

    for (size_t index = 0U; index < client_count; ++index)
    {
        const esp_err_t close_error = httpd_sess_trigger_close(server, client_fds[index]);
        if (close_error != ESP_OK && close_error != ESP_ERR_NOT_FOUND)
        {
            ESP_LOGW(TAG, "触发关闭 HTTP 客户端失败，将等待 handler 自行退出: %s", esp_err_to_name(close_error));
        }
    }
}

/**
 * @brief 在 Service 状态锁内检查所有 handler 是否已经退出
 *
 * @return true 活动 handler 计数为零；false 仍有 handler 尚未配对退出
 */
static bool web_file_handlers_are_drained(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    const bool drained = s_context.active_handlers == 0U;
    xSemaphoreGive(s_context.lock);
    return drained;
}

/**
 * @brief 把单调时钟绝对期限换算为不超过剩余预算的等待 tick
 *
 * @param[in] deadline_us 单调时钟绝对期限
 * @return 可用于 FreeRTOS 等待 API 的剩余 tick；0 表示期限不足一个 tick
 */
static TickType_t web_file_remaining_wait_ticks(int64_t deadline_us)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us >= deadline_us)
    {
        return 0U;
    }

    const uint64_t remaining_ms = (uint64_t) (deadline_us - now_us) / 1000U;
    uint64_t       wait_ticks   = remaining_ms * (uint64_t) configTICK_RATE_HZ / 1000U;
    if (wait_ticks >= (uint64_t) portMAX_DELAY)
    {
        wait_ticks = (uint64_t) portMAX_DELAY - 1U;
    }
    return (TickType_t) wait_ticks;
}

/**
 * @brief 为当前 Service 持有的 HTTPD 句柄启动唯一清理 Task
 *
 * 本函数先在锁内保留清理所有权，再在锁外创建 Task，避免两个生命周期调用同时销毁同一
 * HTTPD。历史完成信号会在创建前排空；完成条件始终是锁内结果标志，不依赖信号本身。
 *
 * @return ESP_OK Task 已创建并独占 HTTPD；ESP_ERR_INVALID_STATE 清理状态不允许启动；
 *         ESP_ERR_NO_MEM Task 创建失败
 */
static esp_err_t web_file_start_httpd_stop_task(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    if (s_context.server == NULL || s_context.httpd_stop_task != NULL || s_context.httpd_stop_in_progress
        || s_context.httpd_stop_result_ready)
    {
        xSemaphoreGive(s_context.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_context.httpd_stop_in_progress = true;
    s_context.httpd_stop_result      = ESP_OK;
    SemaphoreHandle_t completion     = s_context.httpd_stop_completed;
    xSemaphoreGive(s_context.lock);

    while (xSemaphoreTake(completion, 0U) == pdTRUE)
    {
    }

    TaskHandle_t task  = NULL;
    esp_err_t    error = web_file_httpd_stop_task_create(&s_context, &task);

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    if (error == ESP_OK)
    {
        s_context.httpd_stop_task = task;
    }
    else
    {
        s_context.httpd_stop_in_progress = false;
        s_context.httpd_stop_result      = error;
    }
    xSemaphoreGive(s_context.lock);
    return error;
}

/**
 * @brief 在绝对期限内等待一次性 HTTPD 清理 Task 发布结果
 *
 * 二值信号量只用于唤醒，`httpd_stop_result_ready` 才是完成条件。等待最多按一个 tick
 * 分片复查，覆盖“完成信号已被先前超时调用消费、结果随后才发布”的窗口。调用超时不会删除
 * Task、清空服务器句柄或释放其同步资源；后台清理可以继续，后续 `stop()` 等待同一所有者。
 *
 * @param[in] deadline_us 从外层生命周期 API 入口计算的单调时钟绝对期限
 * @return ESP_OK 结果已经发布；ESP_ERR_TIMEOUT 总期限耗尽；
 *         ESP_ERR_INVALID_STATE 不存在进行中或待收取的清理
 */
static esp_err_t web_file_wait_for_httpd_stop_result(int64_t deadline_us)
{
    for (;;)
    {
        xSemaphoreTake(s_context.lock, portMAX_DELAY);
        const bool              result_ready = s_context.httpd_stop_result_ready;
        const bool              in_progress  = s_context.httpd_stop_in_progress;
        const SemaphoreHandle_t completion   = s_context.httpd_stop_completed;
        xSemaphoreGive(s_context.lock);

        if (result_ready)
        {
            return ESP_OK;
        }
        if (!in_progress)
        {
            return ESP_ERR_INVALID_STATE;
        }

        const TickType_t wait_ticks = web_file_remaining_wait_ticks(deadline_us);
        if (wait_ticks == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }
        const TickType_t wait_slice = wait_ticks > 1U ? 1U : wait_ticks;
        (void) xSemaphoreTake(completion, wait_slice);
    }
}

/**
 * @brief 收取 HTTPD 清理结果并删除已经停止访问 Service 状态的一次性 Task
 *
 * 清理 Task 在结果可见后不再访问 Service 状态或同步对象。调用方在锁外删除 Task，随后才
 * 清空句柄和结果标志，使 `deinit()` 不会与 Task 或完成信号产生 UAF。
 *
 * @param[out] out_result `httpd_stop()` 的原始结果
 * @return ESP_OK Task 已回收且结果有效；ESP_ERR_INVALID_ARG 输出为空；
 *         ESP_ERR_INVALID_STATE 结果尚未发布或 Task 句柄缺失
 */
static esp_err_t web_file_collect_httpd_stop_result(esp_err_t *out_result)
{
    if (out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    if (!s_context.httpd_stop_result_ready || s_context.httpd_stop_in_progress || s_context.httpd_stop_task == NULL)
    {
        xSemaphoreGive(s_context.lock);
        return ESP_ERR_INVALID_STATE;
    }
    const TaskHandle_t task   = s_context.httpd_stop_task;
    const esp_err_t    result = s_context.httpd_stop_result;
    xSemaphoreGive(s_context.lock);

    vTaskDelete(task);

    xSemaphoreTake(s_context.lock, portMAX_DELAY);
    s_context.httpd_stop_task         = NULL;
    s_context.httpd_stop_result       = ESP_OK;
    s_context.httpd_stop_result_ready = false;
    xSemaphoreGive(s_context.lock);

    *out_result = result;
    return ESP_OK;
}

/**
 * @brief 在绝对期限内等待 handler 计数归零
 *
 * 二值信号量只用于唤醒，活动计数才是完成条件，因此历史遗留信号或零到非零的竞争不会产生
 * 假排空。每次唤醒后都会在状态锁内重新检查计数。本函数只等待 handler，不停止 HTTPD；
 * 外层生命周期流程会把同一 `deadline_us` 继续用于等待一次性 HTTPD 清理 Task。
 *
 * @param[in] deadline_us 单调时钟绝对期限
 * @return ESP_OK handler 已排空；ESP_ERR_TIMEOUT 期限耗尽
 */
static esp_err_t web_file_wait_for_handlers(int64_t deadline_us)
{
    while (!web_file_handlers_are_drained())
    {
        const TickType_t wait_ticks = web_file_remaining_wait_ticks(deadline_us);
        if (wait_ticks == 0U)
        {
            return ESP_ERR_TIMEOUT;
        }
        (void) xSemaphoreTake(s_context.handlers_drained, wait_ticks);
    }
    return ESP_OK;
}

esp_err_t web_console_service_stop(uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t lock = s_context.lock;
    if (lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;

    xSemaphoreTake(lock, portMAX_DELAY);
    const bool stoppable           = s_context.state == WEB_CONSOLE_SERVICE_STATE_RUNNING
                                     || s_context.state == WEB_CONSOLE_SERVICE_STATE_STOPPING
                                     || s_context.state == WEB_CONSOLE_SERVICE_STATE_CLEANUP_FAILED;
    const bool cleanup_owned       = s_context.httpd_stop_in_progress || s_context.httpd_stop_result_ready;
    const bool cleanup_state_valid = cleanup_owned ? s_context.httpd_stop_task != NULL
                                                   : s_context.server != NULL && s_context.httpd_stop_task == NULL;
    if (!stoppable || s_context.lifecycle_active || !cleanup_state_valid)
    {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_context.lifecycle_active   = true;
    s_context.state              = WEB_CONSOLE_SERVICE_STATE_STOPPING;
    s_context.accepting_requests = false;
    web_file_auth_reset(&s_context.auth, NULL);
    httpd_handle_t server = s_context.server;
    xSemaphoreGive(lock);

    esp_err_t error = ESP_OK;
    if (cleanup_owned)
    {
        error = web_file_wait_for_httpd_stop_result(deadline_us);
    }
    else
    {
        web_file_trigger_client_close(server);
        error = web_console_http_close_ingress(server, deadline_us);
        if (error == ESP_OK)
        {
            error = web_file_wait_for_handlers(deadline_us);
        }
        if (error == ESP_OK)
        {
#if CONFIG_WEB_CONSOLE_FILES
            web_console_files_cleanup_after_handlers();
#endif
            error =
                web_file_remaining_wait_ticks(deadline_us) == 0U ? ESP_ERR_TIMEOUT : web_file_start_httpd_stop_task();
        }
        if (error == ESP_OK)
        {
            error = web_file_wait_for_httpd_stop_result(deadline_us);
        }
    }

    if (error == ESP_OK)
    {
        esp_err_t stop_result;
        error = web_file_collect_httpd_stop_result(&stop_result);
        if (error == ESP_OK)
        {
            error = stop_result;
        }
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    s_context.lifecycle_active = false;
    if (error == ESP_OK)
    {
        s_context.server                  = NULL;
        s_context.state                   = WEB_CONSOLE_SERVICE_STATE_INITIALIZED;
        s_context.registered_route_count = 0U;
        s_context.ingress_close_error     = ESP_OK;
        s_context.ingress_close_queued    = false;
        s_context.last_error              = ESP_OK;
    }
    else if (error == ESP_ERR_TIMEOUT)
    {
        s_context.state      = WEB_CONSOLE_SERVICE_STATE_STOPPING;
        s_context.last_error = error;
    }
    else
    {
        s_context.state      = WEB_CONSOLE_SERVICE_STATE_CLEANUP_FAILED;
        s_context.last_error = error;
    }
    xSemaphoreGive(lock);
    return error;
}

esp_err_t web_console_service_get_status_copy(web_console_service_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t lock = s_context.lock;
    if (lock == NULL)
    {
        memset(out_status, 0, sizeof(*out_status));
        out_status->state = WEB_CONSOLE_SERVICE_STATE_UNINITIALIZED;
        return ESP_OK;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    out_status->state          = s_context.state;
    out_status->session_active = s_context.auth.session_active;
    memcpy(out_status->access_code, s_context.auth.access_code, sizeof(out_status->access_code));
    out_status->last_error = s_context.last_error;
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t web_console_service_deinit(void)
{
    SemaphoreHandle_t lock = s_context.lock;
    if (lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    const bool resources_released = s_context.state == WEB_CONSOLE_SERVICE_STATE_INITIALIZED && !s_context.lifecycle_active
                                    && s_context.server == NULL && s_context.active_handlers == 0U
#if CONFIG_WEB_CONSOLE_FILES
                                    && web_console_files_is_idle_locked()
#endif
                                    && web_console_http_routes_are_released_locked()
                                    && !s_context.ingress_close_queued && s_context.httpd_stop_task == NULL
                                    && !s_context.httpd_stop_in_progress && !s_context.httpd_stop_result_ready;
    if (!resources_released)
    {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t handlers_drained     = s_context.handlers_drained;
    SemaphoreHandle_t ingress_closed       = s_context.ingress_closed;
    SemaphoreHandle_t httpd_stop_completed = s_context.httpd_stop_completed;
    s_context.state                        = WEB_CONSOLE_SERVICE_STATE_UNINITIALIZED;
    s_context.lock                         = NULL;
    s_context.handlers_drained             = NULL;
    s_context.ingress_closed               = NULL;
    s_context.httpd_stop_completed         = NULL;
    xSemaphoreGive(lock);

    vSemaphoreDelete(httpd_stop_completed);
    vSemaphoreDelete(ingress_closed);
    vSemaphoreDelete(handlers_drained);
    vSemaphoreDelete(lock);
    memset(&s_context, 0, sizeof(s_context));
    s_context.state = WEB_CONSOLE_SERVICE_STATE_UNINITIALIZED;
#if CONFIG_WEB_CONSOLE_FILES
    web_console_files_reset_context();
#endif
    return ESP_OK;
}
