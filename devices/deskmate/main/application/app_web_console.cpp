/*
 * 文件职责：提供网页控制台公共 C ABI，并拥有产品状态与 Presenter 推送边界。
 */
#include "app_web_console.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_network.h"
#include "app_web_console_internal.hpp"
#include "app_web_console_provider.h"
#include "connect.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "presentation_dispatch.h"
#include "sdkconfig.h"
#if CONFIG_WEB_CONSOLE_FILES
#include "system_filesystem.h"
#include "web_console_files.h"
#endif
#include "web_console_presenter.h"
#include "web_console_service.h"

static const char *TAG                    = "app_web_console";

static portMUX_TYPE          s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static app_web_console_status_t s_status;
static bool                  s_initialized;
static bool                  s_service_cleanup_required;
static uint32_t              s_lease_generation;
static uint64_t              s_presentation_revision;
static StaticSemaphore_t     s_presenter_push_mutex_storage;
static SemaphoreHandle_t     s_presenter_push_mutex;
static bool                  s_status_update_dispatch_pending;

#if CONFIG_WEB_CONSOLE_FILES
/**
 * @brief 把 DeskMate 文件系统容量快照适配为 Console Files Provider
 *
 * 本回调在 Console HTTPD handler 的普通 Task 上下文同步执行，不回调 Console API。
 *
 * @param[in] context 未使用的 Provider 上下文
 * @param[out] out_capacity Console Files 容量快照，仅在回调期间有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；其他值来自系统文件服务
 */
static esp_err_t get_web_console_files_capacity_copy(void *context, web_console_files_capacity_t *out_capacity)
{
    (void) context;
    if (out_capacity == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    system_filesystem_info_t info;
    const esp_err_t          error = system_filesystem_get_info_copy(&info);
    if (error != ESP_OK)
    {
        return error;
    }

    out_capacity->total_bytes = info.total_bytes;
    out_capacity->free_bytes  = info.free_bytes;
    return ESP_OK;
}

static const web_console_files_config_t s_web_console_files_config = {
    .storage =
        {
            .mount_root        = SYSTEM_FILESYSTEM_MOUNT_POINT,
            .get_capacity_copy = get_web_console_files_capacity_copy,
            .context           = NULL,
        },
    .workspace_name      = ".deskmate-web",
    .upload_max_bytes    = 500ULL * 1024ULL * 1024ULL,
    .reserved_free_bytes = 1ULL * 1024ULL * 1024ULL,
};
#endif

/**
 * @brief 初始化串行化 Presenter 更新与事件派发的静态互斥量
 *
 * @return ESP_OK 互斥量可用；ESP_ERR_NO_MEM 静态互斥量创建失败
 */
static esp_err_t initialize_presenter_push_mutex(void)
{
    if (s_presenter_push_mutex == NULL)
    {
        s_presenter_push_mutex = xSemaphoreCreateMutexStatic(&s_presenter_push_mutex_storage);
    }
    if (s_presenter_push_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建网页控制台展示推送互斥量失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** @brief 把 Application 产品状态映射为不反向依赖 Application 的 Presenter 状态 */
static web_console_presenter_state_t map_presenter_state(app_web_console_state_t state)
{
    switch (state)
    {
        case APP_WEB_CONSOLE_STATE_STOPPED:
            return WEB_CONSOLE_PRESENTER_STATE_STOPPED;
        case APP_WEB_CONSOLE_STATE_CHECKING_STORAGE:
            return WEB_CONSOLE_PRESENTER_STATE_CHECKING_STORAGE;
        case APP_WEB_CONSOLE_STATE_ACQUIRING_NETWORK:
            return WEB_CONSOLE_PRESENTER_STATE_ACQUIRING_NETWORK;
        case APP_WEB_CONSOLE_STATE_STARTING_SERVICE:
            return WEB_CONSOLE_PRESENTER_STATE_STARTING_SERVICE;
        case APP_WEB_CONSOLE_STATE_RUNNING:
            return WEB_CONSOLE_PRESENTER_STATE_RUNNING;
        case APP_WEB_CONSOLE_STATE_STOPPING:
            return WEB_CONSOLE_PRESENTER_STATE_STOPPING;
        case APP_WEB_CONSOLE_STATE_ERROR:
        default:
            return WEB_CONSOLE_PRESENTER_STATE_ERROR;
    }
}

/**
 * @brief 从当前内存链路快照格式化本地 URL，链路不可用时输出空串
 *
 * @param[out] out_url 固定 URL 输出缓冲区
 */
static void make_current_url(char out_url[sizeof(s_status.url)])
{
    out_url[0] = '\0';
    connect_link_info_t link;
    if (connect_get_link_snapshot_copy(&link) == ESP_OK && link.associated && link.has_ipv4 && link.ip[0] != '\0')
    {
        (void) snprintf(out_url, sizeof(s_status.url), "http://%s/", link.ip);
    }
}

/** @brief 验证 Service 快照中的访问码严格为六位十进制数字 */
static bool access_code_is_valid(const char access_code[7])
{
    for (size_t index = 0U; index < 6U; ++index)
    {
        if (access_code[index] < '0' || access_code[index] > '9')
        {
            return false;
        }
    }
    return access_code[6] == '\0';
}

/**
 * @brief 接收网络 Application 的快速通知并转交网页控制台 Task 合并处理
 *
 * @param[in] context 固件进程期静态上下文，本实现不使用
 */
static void on_network_link_change(void *context)
{
    (void) context;
    app_web_console_task_notify_link_change();
}

/**
 * @brief 在状态锁内构造不含所有权句柄的纯展示事实
 *
 * @param[out] out_input Presenter 输入副本
 */
static void make_presenter_input_locked(web_console_presenter_input_t *out_input)
{
    memset(out_input, 0, sizeof(*out_input));
    out_input->presentation_revision = s_presentation_revision;
    out_input->state                 = map_presenter_state(s_status.state);
    out_input->exit_allowed =
        s_status.state == APP_WEB_CONSOLE_STATE_STOPPED
        || (s_status.state == APP_WEB_CONSOLE_STATE_ERROR && !s_service_cleanup_required && s_lease_generation == 0U);
    (void) memcpy(out_input->url, s_status.url, sizeof(out_input->url));
    (void) memcpy(out_input->access_code, s_status.access_code, sizeof(out_input->access_code));
    out_input->total_bytes = s_status.total_bytes;
    out_input->free_bytes  = s_status.free_bytes;
    out_input->error       = s_status.last_error;
}

/**
 * @brief 在展示推送互斥量内更新 Presenter 并处理状态事件 pending
 *
 * `synchronous_rejection` 只用于 Task 尚未发布时的同步拒绝：若本版本被 Presenter 接受，
 * 清除此前尚未进入 Event Loop 的准备态 pending，且不为拒绝结果新增异步终态事件。若本版本
 * 未被接受，则保留并重试属于更高版本的 pending，避免误清其他 Task 的结果。
 *
 * @param[in] dispatch true 表示异步产品状态发生变化，需要通知 UI 重新读取
 * @param[in] synchronous_rejection true 表示调用方将直接处理同步返回错误
 * @return true Presenter 接受本次新版本；false 版本耗尽、更新失败或输入已过时
 */
static bool push_presenter_status_locked(bool dispatch, bool synchronous_rejection)
{
    web_console_presenter_input_t input;
    taskENTER_CRITICAL(&s_state_lock);
    if (s_presentation_revision == UINT64_MAX)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        ESP_LOGE(TAG, "网页控制台展示版本已耗尽，拒绝继续推送");
        return false;
    }
    ++s_presentation_revision;
    make_presenter_input_locked(&input);
    taskEXIT_CRITICAL(&s_state_lock);

    bool            accepted     = false;
    const esp_err_t update_error = web_console_presenter_update_copy(&input, &accepted);
    if (update_error != ESP_OK)
    {
        ESP_LOGW(TAG, "推送网页控制台展示状态失败: %s", esp_err_to_name(update_error));
        return false;
    }

    if (synchronous_rejection && accepted)
    {
        s_status_update_dispatch_pending = false;
    }
    else if (dispatch && accepted)
    {
        s_status_update_dispatch_pending = true;
    }
    if (s_status_update_dispatch_pending)
    {
        const esp_err_t dispatch_error = presentation_dispatch_status_update();
        if (dispatch_error == ESP_OK)
        {
            s_status_update_dispatch_pending = false;
        }
        else
        {
            ESP_LOGW(TAG, "发布网页控制台状态更新失败: %s", esp_err_to_name(dispatch_error));
        }
    }
    return accepted;
}

/**
 * @brief 串行推送纯展示事实，并耐久发布轻量呈现事件
 *
 * 私有互斥量覆盖 Presenter 版本仲裁、pending 标志与事件入队，保证任何更高版本只能在当前
 * 版本完成派发尝试后生效。Presenter 接受的新版本先置 pending，只有默认 Event Loop 接受
 * `STATUS_UPDATE` 后才清除；Application 状态锁仅用于分配版本和复制快照，不跨越 Presenter
 * 或事件调用。
 *
 * @param[in] dispatch true 表示产品状态发生变化，需要通知 UI 重新读取
 * @return true Presenter 接受本次新版本；false 版本耗尽、更新失败或输入已过时
 */
static bool push_presenter_status(bool dispatch)
{
    if (s_presenter_push_mutex == NULL || xSemaphoreTake(s_presenter_push_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "获取网页控制台展示推送互斥量失败");
        return false;
    }
    const bool accepted = push_presenter_status_locked(dispatch, false);
    (void) xSemaphoreGive(s_presenter_push_mutex);
    return accepted;
}

esp_err_t app_web_console_internal_retry_status_update(void)
{
    if (s_presenter_push_mutex == NULL || xSemaphoreTake(s_presenter_push_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = ESP_OK;
    if (s_status_update_dispatch_pending)
    {
        error = presentation_dispatch_status_update();
        if (error == ESP_OK)
        {
            s_status_update_dispatch_pending = false;
        }
    }
    (void) xSemaphoreGive(s_presenter_push_mutex);
    return error;
}

esp_err_t app_web_console_internal_validate_stop_request(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool initialized = s_initialized;
    taskEXIT_CRITICAL(&s_state_lock);
    return initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t app_web_console_internal_prepare_start(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || (s_status.state != APP_WEB_CONSOLE_STATE_STOPPED && s_status.state != APP_WEB_CONSOLE_STATE_ERROR)
        || s_service_cleanup_required || s_lease_generation != 0U || s_presentation_revision == UINT64_MAX)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.state          = APP_WEB_CONSOLE_STATE_CHECKING_STORAGE;
    s_status.last_error     = ESP_OK;
    s_status.url[0]         = '\0';
    s_status.access_code[0] = '\0';
    taskEXIT_CRITICAL(&s_state_lock);
    push_presenter_status(true);
    return ESP_OK;
}

esp_err_t app_web_console_internal_prepare_stop_retry(bool *out_needs_task)
{
    if (out_needs_task == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool changed = false;
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_status.state == APP_WEB_CONSOLE_STATE_STOPPED)
    {
        *out_needs_task = false;
    }
    else if (s_service_cleanup_required || s_lease_generation != 0U)
    {
        s_status.state          = APP_WEB_CONSOLE_STATE_STOPPING;
        s_status.last_error     = ESP_OK;
        s_status.url[0]         = '\0';
        s_status.access_code[0] = '\0';
        *out_needs_task         = true;
        changed                 = true;
    }
    else
    {
        s_status.state          = APP_WEB_CONSOLE_STATE_STOPPING;
        s_status.last_error     = ESP_OK;
        s_status.url[0]         = '\0';
        s_status.access_code[0] = '\0';
        *out_needs_task         = true;
        changed                 = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (changed)
    {
        push_presenter_status(true);
    }
    return ESP_OK;
}

void app_web_console_internal_publish_state(app_web_console_state_t state, esp_err_t error, bool clear_runtime)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_status.state      = state;
    s_status.last_error = error;
    if (clear_runtime)
    {
        s_status.url[0]         = '\0';
        s_status.access_code[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_state_lock);
    push_presenter_status(true);
}

void app_web_console_internal_publish_synchronous_rejection(esp_err_t error)
{
    if (s_presenter_push_mutex == NULL || xSemaphoreTake(s_presenter_push_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "获取网页控制台同步拒绝展示互斥量失败");
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_status.state          = APP_WEB_CONSOLE_STATE_ERROR;
    s_status.last_error     = error;
    s_status.url[0]         = '\0';
    s_status.access_code[0] = '\0';
    taskEXIT_CRITICAL(&s_state_lock);

    if (!push_presenter_status_locked(false, true))
    {
        ESP_LOGW(TAG, "更新网页控制台同步拒绝展示失败: %s", esp_err_to_name(error));
    }
    (void) xSemaphoreGive(s_presenter_push_mutex);
}

esp_err_t app_web_console_internal_publish_running_snapshot(void)
{
    char url[sizeof(s_status.url)]{};
    make_current_url(url);

    web_console_service_status_t service_status;
    const esp_err_t           service_error = web_console_service_get_status_copy(&service_status);
    if (service_error != ESP_OK)
    {
        return service_error;
    }
    if (service_status.state != WEB_CONSOLE_SERVICE_STATE_RUNNING || !access_code_is_valid(service_status.access_code))
    {
        memset(&service_status, 0, sizeof(service_status));
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || s_status.state != APP_WEB_CONSOLE_STATE_STARTING_SERVICE || !s_service_cleanup_required
        || s_lease_generation == 0U)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        memset(&service_status, 0, sizeof(service_status));
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state      = APP_WEB_CONSOLE_STATE_RUNNING;
    s_status.last_error = ESP_OK;
    memcpy(s_status.url, url, sizeof(s_status.url));
    memcpy(s_status.access_code, service_status.access_code, sizeof(s_status.access_code));
    taskEXIT_CRITICAL(&s_state_lock);
    memset(&service_status, 0, sizeof(service_status));

    return push_presenter_status(true) ? ESP_OK : ESP_FAIL;
}

esp_err_t app_web_console_internal_refresh_running_link(void)
{
    char url[sizeof(s_status.url)]{};
    make_current_url(url);

    bool changed = false;
    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized || s_status.state != APP_WEB_CONSOLE_STATE_RUNNING)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (memcmp(s_status.url, url, sizeof(s_status.url)) != 0)
    {
        memcpy(s_status.url, url, sizeof(s_status.url));
        changed = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (!changed)
    {
        return ESP_OK;
    }
    return push_presenter_status(true) ? ESP_OK : ESP_FAIL;
}

void app_web_console_internal_set_capacity(uint64_t total_bytes, uint64_t free_bytes)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_status.total_bytes = total_bytes;
    s_status.free_bytes  = free_bytes;
    taskEXIT_CRITICAL(&s_state_lock);
}

void app_web_console_internal_set_lease_generation(uint32_t generation)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_lease_generation = generation;
    taskEXIT_CRITICAL(&s_state_lock);
}

uint32_t app_web_console_internal_get_lease_generation(void)
{
    uint32_t generation;
    taskENTER_CRITICAL(&s_state_lock);
    generation = s_lease_generation;
    taskEXIT_CRITICAL(&s_state_lock);
    return generation;
}

void app_web_console_internal_set_service_cleanup_required(bool required)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_service_cleanup_required = required;
    taskEXIT_CRITICAL(&s_state_lock);
}

app_web_console_state_t app_web_console_internal_get_state(void)
{
    app_web_console_state_t state;
    taskENTER_CRITICAL(&s_state_lock);
    state = s_status.state;
    taskEXIT_CRITICAL(&s_state_lock);
    return state;
}

esp_err_t app_web_console_internal_initialize_service(void)
{
    size_t settings_provider_count = 0U;
    size_t status_provider_count   = 0U;
    const web_console_settings_provider_t *settings_providers =
        app_web_console_provider_get_settings_borrow(&settings_provider_count);
    const web_console_status_provider_t *status_providers =
        app_web_console_provider_get_status_borrow(&status_provider_count);
#if CONFIG_WEB_CONSOLE_ACTIONS
    size_t action_provider_count = 0U;
    const web_console_action_provider_t *action_providers =
        app_web_console_provider_get_actions_borrow(&action_provider_count);
#endif
    const web_console_service_config_t config = {
        .server_port = 80U,
#if CONFIG_WEB_CONSOLE_FILES
        .files = &s_web_console_files_config,
#else
        .files = NULL,
#endif
        .settings_providers      = settings_providers,
        .settings_provider_count = settings_provider_count,
        .status_providers        = status_providers,
        .status_provider_count   = status_provider_count,
#if CONFIG_WEB_CONSOLE_ACTIONS
        .action_providers        = action_providers,
        .action_provider_count   = action_provider_count,
#endif
    };
    return web_console_service_init_borrow(&config);
}

esp_err_t app_web_console_init(void)
{
    const esp_err_t mutex_error = initialize_presenter_push_mutex();
    if (mutex_error != ESP_OK)
    {
        return mutex_error;
    }

    web_console_service_status_t service_status;
    const esp_err_t           error = web_console_service_get_status_copy(&service_status);
    if (error != ESP_OK)
    {
        return error;
    }
    if (service_status.state == WEB_CONSOLE_SERVICE_STATE_UNINITIALIZED)
    {
        const esp_err_t init_error = app_web_console_internal_initialize_service();
        if (init_error != ESP_OK)
        {
            return init_error;
        }
        service_status.state = WEB_CONSOLE_SERVICE_STATE_INITIALIZED;
    }
    if (service_status.state != WEB_CONSOLE_SERVICE_STATE_INITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized)
    {
        const bool safely_initialized =
            s_status.state == APP_WEB_CONSOLE_STATE_STOPPED && !s_service_cleanup_required && s_lease_generation == 0U;
        taskEXIT_CRITICAL(&s_state_lock);
        if (safely_initialized)
        {
            const esp_err_t callback_error =
                app_network_register_link_change_callback_borrow(on_network_link_change, NULL);
            if (callback_error != ESP_OK)
            {
                return callback_error;
            }
            push_presenter_status(false);
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state             = APP_WEB_CONSOLE_STATE_STOPPED;
    s_status.last_error        = ESP_OK;
    s_service_cleanup_required = false;
    s_lease_generation         = 0U;
    s_presentation_revision    = 0U;
    s_initialized              = true;
    taskEXIT_CRITICAL(&s_state_lock);
    const esp_err_t callback_error = app_network_register_link_change_callback_borrow(on_network_link_change, NULL);
    if (callback_error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        memset(&s_status, 0, sizeof(s_status));
        s_service_cleanup_required       = false;
        s_lease_generation               = 0U;
        s_presentation_revision          = 0U;
        s_status_update_dispatch_pending = false;
        s_initialized                    = false;
        taskEXIT_CRITICAL(&s_state_lock);
        return callback_error;
    }
    push_presenter_status(false);
    return ESP_OK;
}

esp_err_t app_web_console_request_start(void)
{
    return app_web_console_task_request_start();
}

esp_err_t app_web_console_request_stop(void)
{
    return app_web_console_task_request_stop();
}

esp_err_t app_web_console_get_status_copy(app_web_console_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (!s_initialized)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_status = s_status;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}
