/**
 * @file power_management_app.cpp
 * @brief 实现电源管理 App 公共生命周期、事件订阅和启动失败休眠入口
 */
#include "power_management_app.h"

#include "content_refresh_app.h"
#include "device_power.h"
#include "esp_check.h"
#include "esp_log.h"
#include "photo_playback_app.h"
#include "power_management_app_internal.hpp"

/** @brief 日志标签 */
static const char *TAG = "power_management_app";

/** @brief 电源管理 App 唯一 Runtime */
PowerManagementRuntime g_power_management_runtime;

void power_management_app_publish(power_management_app_state_t state, esp_err_t error)
{
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    g_power_management_runtime.status.state      = state;
    g_power_management_runtime.status.last_error = error;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
}

/**
 * @brief 提交确认键完整内容刷新，并通知电源 Task 暂停当前无活动窗口
 *
 * @return ESP_OK 刷新与电源通知均已提交；或生命周期、刷新通知错误码
 */
static esp_err_t power_management_app_on_refresh_request(void *context)
{
    (void) context;
    content_refresh_app_status_t refresh_status = {};
    esp_err_t refresh_error = content_refresh_app_get_status_copy(&refresh_status);
    if (refresh_error == ESP_OK && refresh_status.state == CONTENT_REFRESH_APP_STATE_STOPPED)
    {
        refresh_error = content_refresh_app_start();
    }
    else if (refresh_error == ESP_OK)
    {
        refresh_error = content_refresh_app_request_refresh();
    }
    if (refresh_error != ESP_OK)
    {
        return refresh_error;
    }

    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    task = g_power_management_runtime.task;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    if (task == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_MANUAL_REFRESH, eSetBits) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

/** @brief 把左键三秒长按转换为一次性固件检查通知 */
static esp_err_t power_management_app_on_firmware_check_request(void *context)
{
    (void) context;
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    task = g_power_management_runtime.task;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    ESP_RETURN_ON_FALSE(task != nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 Task 尚未运行，不能检查固件");
    ESP_RETURN_ON_FALSE(xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_FIRMWARE_CHECK, eSetBits)
                            == pdPASS,
                        ESP_FAIL,
                        TAG,
                        "提交固件检查通知失败");
    return ESP_OK;
}

/** @brief 把物理中键三秒长按转换为一次性配网通知 */
static esp_err_t power_management_app_on_provisioning_request(void *context)
{
    (void) context;
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    task = g_power_management_runtime.task;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    ESP_RETURN_ON_FALSE(task != nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 Task 尚未运行，不能请求配网");
    return xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_PROVISIONING, eSetBits) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

/** @brief 把照片播放 App 的模态按键动作转换为电源管理通知 */
static esp_err_t power_management_app_on_modal_action(
    photo_playback_app_modal_action_t action, void *context)
{
    (void) context;
    TaskHandle_t task;
    power_management_app_state_t state;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    task = g_power_management_runtime.task;
    state = g_power_management_runtime.status.state;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_INVALID_STATE, TAG, "电源管理 Task 尚未运行");
    if (state == POWER_MANAGEMENT_APP_STATE_INSTALLING)
    {
        return ESP_OK;
    }
    const uint32_t notification = action == PHOTO_PLAYBACK_APP_MODAL_ACTION_LEFT
                                      ? POWER_MANAGEMENT_NOTIFY_MODAL_LEFT
                                      : POWER_MANAGEMENT_NOTIFY_MODAL_CONFIRM;
    return xTaskNotify(task, notification, eSetBits) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t power_management_app_begin_ota_modal_capture(void)
{
    return photo_playback_app_begin_modal_borrow(power_management_app_on_modal_action, nullptr);
}

/** @brief Network Manager 状态变化时唤醒 OTA 网络等待 */
void power_management_app_on_network_change(void *context)
{
    (void) context;
    if (g_power_management_runtime.network_changed != nullptr)
    {
        (void) xSemaphoreGive(g_power_management_runtime.network_changed);
    }
}

/** @brief 把 OTA Task 的不可变完成事件复制给电源管理 Task */
static void power_management_app_on_firmware_ota_event(const firmware_ota_event_t *event,
                                                       void *context)
{
    (void) context;
    if (event == nullptr)
    {
        ESP_LOGE(TAG, "固件 OTA 完成事件为空");
        return;
    }
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    g_power_management_runtime.latest_firmware_ota_event = *event;
    g_power_management_runtime.firmware_ota_event_pending = true;
    task = g_power_management_runtime.task;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    if (task != nullptr)
    {
        (void) xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_FIRMWARE_OTA_EVENT, eSetBits);
    }
}

/** @brief 把内容刷新完成事件复制给电源管理 Task */
static void power_management_app_on_refresh_round(
    const content_refresh_app_round_event_t *event, void *context)
{
    (void) context;
    if (event == nullptr)
    {
        ESP_LOGE(TAG, "内容刷新轮次事件为空");
        return;
    }
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    g_power_management_runtime.latest_round_event = *event;
    g_power_management_runtime.round_event_pending = true;
    task = g_power_management_runtime.task;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    if (task != nullptr)
    {
        (void) xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_REFRESH_ROUND, eSetBits);
    }
}

/** @brief 通知电源管理 Task 重新检查照片集合显示状态 */
static void power_management_app_on_display_settled(
    const photo_playback_app_collection_settled_event_t *event, void *context)
{
    (void) event;
    (void) context;
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    task = g_power_management_runtime.task;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    if (task != nullptr)
    {
        (void) xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_DISPLAY_SETTLED, eSetBits);
    }
}

/** @brief 通知电源管理 Task 重置三分钟无活动窗口 */
static void power_management_app_on_user_activity(void *context)
{
    (void) context;
    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    task = g_power_management_runtime.task;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    if (task != nullptr)
    {
        (void) xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_USER_ACTIVITY, eSetBits);
    }
}

/** @brief 清除本 App 注册的全部借用回调，返回首个错误 */
static esp_err_t power_management_app_clear_callbacks()
{
    esp_err_t first_error = photo_playback_app_set_refresh_request_callback_borrow(nullptr, nullptr);
    const esp_err_t firmware_error =
        photo_playback_app_set_firmware_check_request_callback_borrow(nullptr, nullptr);
    if (first_error == ESP_OK)
    {
        first_error = firmware_error;
    }
    const esp_err_t provisioning_error =
        photo_playback_app_set_provisioning_request_callback_borrow(nullptr, nullptr);
    if (first_error == ESP_OK)
    {
        first_error = provisioning_error;
    }
    const esp_err_t modal_error = photo_playback_app_end_modal();
    if (first_error == ESP_OK)
    {
        first_error = modal_error;
    }
    const esp_err_t settled_error =
        photo_playback_app_set_collection_settled_callback_borrow(nullptr, nullptr);
    if (first_error == ESP_OK)
    {
        first_error = settled_error;
    }
    const esp_err_t activity_error =
        photo_playback_app_set_activity_callback_borrow(nullptr, nullptr);
    if (first_error == ESP_OK)
    {
        first_error = activity_error;
    }
    if (g_power_management_runtime.status.automatic_sleep_enabled)
    {
        esp_err_t refresh_error =
            content_refresh_app_set_round_callback_borrow(nullptr, nullptr);
        if (refresh_error == ESP_ERR_INVALID_STATE)
        {
            refresh_error = ESP_OK;
        }
        if (first_error == ESP_OK)
        {
            first_error = refresh_error;
        }
    }
    const esp_err_t ota_event_error = firmware_ota_set_event_callback_borrow(nullptr, nullptr);
    if (first_error == ESP_OK)
    {
        first_error = ota_event_error;
    }
    return first_error;
}

esp_err_t power_management_app_init(const power_management_app_config_t *config)
{
    ESP_RETURN_ON_FALSE(
        config != nullptr
            && (!config->automatic_sleep_enabled || config->interactive_awake_ms > 0U),
        ESP_ERR_INVALID_ARG,
        TAG,
        "电源管理配置无效");
    ESP_RETURN_ON_FALSE(!g_power_management_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 App 已初始化");

    g_power_management_runtime.task_stopped = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(g_power_management_runtime.task_stopped != nullptr,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "创建电源管理停止信号量失败");
    g_power_management_runtime.network_changed = xSemaphoreCreateBinary();
    if (g_power_management_runtime.network_changed == nullptr)
    {
        vSemaphoreDelete(g_power_management_runtime.task_stopped);
        g_power_management_runtime.task_stopped = nullptr;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret               = ESP_OK;
    bool      timer_wakeup_boot = false;
    ESP_GOTO_ON_ERROR(device_power_was_woken_by_timer(&timer_wakeup_boot),
                      cleanup,
                      TAG,
                      "读取定时唤醒来源失败");
    g_power_management_runtime.interactive_awake_ms = config->interactive_awake_ms;
    g_power_management_runtime.round_event_pending = false;
    g_power_management_runtime.latest_round_event = {};
    g_power_management_runtime.firmware_ota_event_pending = false;
    g_power_management_runtime.latest_firmware_ota_event = {};
    g_power_management_runtime.refresh_start_error = ESP_OK;
    g_power_management_runtime.status = {};
    g_power_management_runtime.status.state = POWER_MANAGEMENT_APP_STATE_STOPPED;
    g_power_management_runtime.status.automatic_sleep_enabled =
        config->automatic_sleep_enabled;
    g_power_management_runtime.status.timer_wakeup_boot = timer_wakeup_boot;
    g_power_management_runtime.initialized = true;
    return ESP_OK;

cleanup:
    vSemaphoreDelete(g_power_management_runtime.network_changed);
    g_power_management_runtime.network_changed = nullptr;
    vSemaphoreDelete(g_power_management_runtime.task_stopped);
    g_power_management_runtime.task_stopped = nullptr;
    return ret;
}

esp_err_t power_management_app_start(void)
{
    ESP_RETURN_ON_FALSE(g_power_management_runtime.initialized
                            && g_power_management_runtime.task == nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 App 当前生命周期不允许启动");
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    const bool stopped = g_power_management_runtime.status.state
                         == POWER_MANAGEMENT_APP_STATE_STOPPED;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    ESP_RETURN_ON_FALSE(stopped, ESP_ERR_INVALID_STATE, TAG, "电源管理 App 尚未达到可启动状态");

    ESP_RETURN_ON_ERROR(power_management_app_task_start(), TAG, "创建电源管理 Task 失败");

    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(
        firmware_ota_set_event_callback_borrow(power_management_app_on_firmware_ota_event,
                                               nullptr),
        cleanup,
        TAG,
        "注册固件 OTA 完成回调失败");
    ESP_GOTO_ON_ERROR(
        photo_playback_app_set_refresh_request_callback_borrow(
            power_management_app_on_refresh_request, nullptr),
        cleanup,
        TAG,
        "注册手动内容刷新请求回调失败");
    ESP_GOTO_ON_ERROR(
        photo_playback_app_set_firmware_check_request_callback_borrow(
            power_management_app_on_firmware_check_request, nullptr),
        cleanup,
        TAG,
        "注册固件检查请求回调失败");
    ESP_GOTO_ON_ERROR(
        photo_playback_app_set_provisioning_request_callback_borrow(
            power_management_app_on_provisioning_request, nullptr),
        cleanup,
        TAG,
        "注册配网请求回调失败");
    ESP_GOTO_ON_ERROR(photo_playback_app_set_collection_settled_callback_borrow(
                          power_management_app_on_display_settled,
                          nullptr),
                      cleanup,
                      TAG,
                      "注册照片集合收敛回调失败");
    ESP_GOTO_ON_ERROR(
        photo_playback_app_set_activity_callback_borrow(power_management_app_on_user_activity,
                                                        nullptr),
        cleanup,
        TAG,
        "注册用户活动回调失败");
    if (g_power_management_runtime.status.automatic_sleep_enabled)
    {
        ESP_GOTO_ON_ERROR(
            content_refresh_app_set_round_callback_borrow(power_management_app_on_refresh_round,
                                                          nullptr),
            cleanup,
            TAG,
            "注册内容刷新轮次回调失败");
    }
    return ESP_OK;

cleanup:
    ESP_ERROR_CHECK_WITHOUT_ABORT(power_management_app_clear_callbacks());
    ESP_ERROR_CHECK_WITHOUT_ABORT(power_management_app_task_stop());
    return ret;
}

esp_err_t power_management_app_request_sleep(void)
{
    ESP_RETURN_ON_FALSE(g_power_management_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 App 尚未初始化");

    TaskHandle_t task;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    const power_management_app_state_t previous_state =
        g_power_management_runtime.status.state;
    const bool accepts = previous_state == POWER_MANAGEMENT_APP_STATE_IDLE
                         || previous_state == POWER_MANAGEMENT_APP_STATE_WAIT_REFRESH
                         || previous_state == POWER_MANAGEMENT_APP_STATE_WAIT_DISPLAY
                          || previous_state == POWER_MANAGEMENT_APP_STATE_AWAKE_WINDOW;
    task = g_power_management_runtime.task;
    if (accepts && task != nullptr)
    {
        g_power_management_runtime.status.state = POWER_MANAGEMENT_APP_STATE_SLEEP_REQUESTED;
        ++g_power_management_runtime.status.accepted_sleep_requests;
    }
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    if (!accepts || task == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_SLEEP, eSetBits) != pdPASS)
    {
        taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
        g_power_management_runtime.status.state      = previous_state;
        g_power_management_runtime.status.last_error = ESP_FAIL;
        --g_power_management_runtime.status.accepted_sleep_requests;
        taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t power_management_app_report_refresh_start_failure(esp_err_t error)
{
    ESP_RETURN_ON_FALSE(error != ESP_OK,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "内容刷新启动失败事实不能为 ESP_OK");
    ESP_RETURN_ON_FALSE(g_power_management_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 App 尚未初始化");

    TaskHandle_t task;
    esp_err_t previous_error;
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    const bool accepts = g_power_management_runtime.status.automatic_sleep_enabled
                         && g_power_management_runtime.status.state
                                == POWER_MANAGEMENT_APP_STATE_WAIT_REFRESH
                         && g_power_management_runtime.task != nullptr;
    task = g_power_management_runtime.task;
    previous_error = g_power_management_runtime.status.last_error;
    if (accepts)
    {
        g_power_management_runtime.refresh_start_error = error;
        g_power_management_runtime.status.state =
            POWER_MANAGEMENT_APP_STATE_RETRY_SLEEP_REQUESTED;
        g_power_management_runtime.status.last_error = error;
    }
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    if (!accepts)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskNotify(task, POWER_MANAGEMENT_NOTIFY_REFRESH_START_FAILURE, eSetBits) != pdPASS)
    {
        taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
        g_power_management_runtime.refresh_start_error = ESP_OK;
        g_power_management_runtime.status.state = POWER_MANAGEMENT_APP_STATE_WAIT_REFRESH;
        g_power_management_runtime.status.last_error = previous_error;
        taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t power_management_app_enter_startup_sleep(
    power_management_app_startup_sleep_policy_t policy, esp_err_t reason)
{
    ESP_RETURN_ON_FALSE(reason != ESP_OK
                            && (policy == POWER_MANAGEMENT_APP_STARTUP_SLEEP_UNTIL_BUTTON
                                || policy == POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "启动阶段深睡参数无效");
    ESP_RETURN_ON_FALSE(!g_power_management_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "运行期电源管理已初始化，不能进入启动阶段深睡路径");
    return power_management_app_prepare_startup_sleep(policy, reason);
}

esp_err_t power_management_app_enforce_absolute_wakeup_gate(bool woken_by_button,
                                                            bool woken_by_timer)
{
    ESP_RETURN_ON_FALSE(!g_power_management_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "运行期电源管理已初始化，不能执行启动绝对时间门禁");
    return power_management_app_apply_startup_time_gate(woken_by_button, woken_by_timer);
}

esp_err_t power_management_app_get_status_copy(power_management_app_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != nullptr,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "电源管理状态输出指针为空");
    ESP_RETURN_ON_FALSE(g_power_management_runtime.initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 App 尚未初始化");
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    *out_status = g_power_management_runtime.status;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    return ESP_OK;
}

esp_err_t power_management_app_stop(void)
{
    ESP_RETURN_ON_FALSE(g_power_management_runtime.initialized
                            && g_power_management_runtime.task != nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 App 当前生命周期不允许停止");
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    const power_management_app_state_t state = g_power_management_runtime.status.state;
    const bool idle = state == POWER_MANAGEMENT_APP_STATE_IDLE
                      || state == POWER_MANAGEMENT_APP_STATE_WAIT_REFRESH
                      || state == POWER_MANAGEMENT_APP_STATE_WAIT_DISPLAY
                      || state == POWER_MANAGEMENT_APP_STATE_AWAKE_WINDOW
                      || state == POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    ESP_RETURN_ON_FALSE(idle, ESP_ERR_INVALID_STATE, TAG, "电源管理 App 当前状态不允许停止");

    ESP_RETURN_ON_ERROR(power_management_app_clear_callbacks(), TAG, "解除电源管理回调失败");
    ESP_RETURN_ON_ERROR(power_management_app_task_stop(), TAG, "停止电源管理 Task 失败");
    return ESP_OK;
}

esp_err_t power_management_app_deinit(void)
{
    ESP_RETURN_ON_FALSE(g_power_management_runtime.initialized
                            && g_power_management_runtime.task == nullptr,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "电源管理 App 当前生命周期不允许反初始化");
    taskENTER_CRITICAL(&g_power_management_runtime.state_lock);
    const bool stopped = g_power_management_runtime.status.state
                         == POWER_MANAGEMENT_APP_STATE_STOPPED;
    taskEXIT_CRITICAL(&g_power_management_runtime.state_lock);
    ESP_RETURN_ON_FALSE(stopped, ESP_ERR_INVALID_STATE, TAG, "电源管理 App 尚未达到已停止状态");

    vSemaphoreDelete(g_power_management_runtime.task_stopped);
    vSemaphoreDelete(g_power_management_runtime.network_changed);
    g_power_management_runtime.task_stopped = nullptr;
    g_power_management_runtime.network_changed = nullptr;
    g_power_management_runtime.interactive_awake_ms = 0U;
    g_power_management_runtime.round_event_pending = false;
    g_power_management_runtime.latest_round_event = {};
    g_power_management_runtime.firmware_ota_event_pending = false;
    g_power_management_runtime.latest_firmware_ota_event = {};
    g_power_management_runtime.refresh_start_error = ESP_OK;
    g_power_management_runtime.status = {};
    g_power_management_runtime.initialized = false;
    return ESP_OK;
}
