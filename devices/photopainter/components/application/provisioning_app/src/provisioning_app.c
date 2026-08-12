/**
 * @file provisioning_app.c
 * @brief 实现网络状态、配网页面和启动失败深睡收敛的同步 Application 用例
 */

#include "provisioning_app.h"

#include <stdbool.h>
#include <stdint.h>

#include "connect.h"
#include "button_service.h"
#include "device_button.h"
#include "device_display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "power_management_app.h"
#include "system_storage.h"
#include "utils.h"

/** @brief 日志标签 */
static const char *TAG = "provisioning_app";
/** @brief Portal 无显式用户活动时的清醒等待时间 */
#define PROVISIONING_APP_PORTAL_IDLE_MS 180000U
/** @brief 网络或 Hub 提示页等待中键长按的交互窗口 */
#define PROVISIONING_APP_PROMPT_IDLE_MS 180000U
/** @brief 已提交 Portal 命令后等待进入配网状态的最长时间 */
#define PROVISIONING_APP_PORTAL_START_MS 30000U
/** @brief 物理中键进入配网所需持续时间 */
#define PROVISIONING_APP_SETUP_HOLD_US  3000000LL
/** @brief 两行状态页的垂直间距 */
#define PROVISIONING_APP_PROMPT_GAP_PIXELS 24U

/** @brief 启动配网用例临时拥有的按键状态 */
typedef struct
{
    TaskHandle_t waiting_task;              /**< 等待网络或按键事实的调用者 Task */
    portMUX_TYPE lock;                      /**< 保护跨 Timer Task 的短状态 */
    int64_t      right_press_started_at_us; /**< 中键有效按下时间 */
    bool         setup_requested;           /**< 已满足三秒长按 */
} provisioning_app_button_context_t;

/** @brief 二维码绘制期间使用的显示坐标上下文 */
typedef struct
{
    device_display_info_t display_info; /**< 显示尺寸与模式快照 */
    uint16_t              origin_x;     /**< 二维码左上角横坐标 */
    uint16_t              origin_y;     /**< 二维码左上角纵坐标 */
} provisioning_app_qr_context_t;

/**
 * @brief 接收二维码布局、清空显示内部帧并计算居中坐标
 *
 * @param[in] layout 二维码布局，仅在回调期间有效
 * @param[in,out] context_ptr 二维码显示上下文
 * @return ESP_OK 内部帧已清空且坐标有效；其他值为参数或显示错误码
 */
static esp_err_t provisioning_app_qr_begin(const connect_qr_layout_t *layout, void *context_ptr)
{
    provisioning_app_qr_context_t *context = context_ptr;
    ESP_RETURN_ON_FALSE(layout != NULL && context != NULL
                            && layout->side_pixels <= context->display_info.width_pixels
                            && layout->side_pixels <= context->display_info.height_pixels,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "配网二维码布局超出显示区域");

    context->origin_x =
        (uint16_t) ((context->display_info.width_pixels - layout->side_pixels) / 2U);
    context->origin_y =
        (uint16_t) ((context->display_info.height_pixels - layout->side_pixels) / 2U);
    return device_display_clear(DEVICE_DISPLAY_TONE_WHITE);
}

/**
 * @brief 把二维码黑色模块映射到显示内部帧的居中坐标
 *
 * @param[in] x_pixels 二维码局部横坐标
 * @param[in] y_pixels 二维码局部纵坐标
 * @param[in] width_pixels 黑色矩形宽度
 * @param[in] height_pixels 黑色矩形高度
 * @param[in] context_ptr 二维码显示上下文
 * @return ESP_OK 已写入内部帧；其他值为参数或显示错误码
 */
static esp_err_t provisioning_app_qr_fill_dark_rect(uint16_t x_pixels, uint16_t y_pixels,
                                                    uint16_t width_pixels, uint16_t height_pixels,
                                                    void *context_ptr)
{
    const provisioning_app_qr_context_t *context = context_ptr;
    ESP_RETURN_ON_FALSE(context != NULL, ESP_ERR_INVALID_ARG, TAG, "配网二维码绘制上下文为空");
    return device_display_fill_rect((uint16_t) (context->origin_x + x_pixels),
                                    (uint16_t) (context->origin_y + y_pixels),
                                    width_pixels,
                                    height_pixels,
                                    DEVICE_DISPLAY_TONE_BLACK);
}

/**
 * @brief 在 network_manager Task 中快速通知等待配网结果的调用者 Task
 *
 * @param[in] context_ptr 等待状态变化的 TaskHandle_t
 */
static void provisioning_app_on_network_change(void *context_ptr)
{
    TaskHandle_t waiting_task = (TaskHandle_t) context_ptr;
    ESP_RETURN_VOID_ON_FALSE(waiting_task != NULL, TAG, "配网等待 Task 句柄为空");
    xTaskNotifyGive(waiting_task);
}

/** @brief 在 Button Service 的 Timer Task 中记录中键三秒长按并唤醒配网用例 */
static void provisioning_app_on_button_event(device_button_id_t button,
                                             device_button_event_t event,
                                             uint8_t click_count,
                                             void *context_ptr)
{
    (void) click_count;
    provisioning_app_button_context_t *context = context_ptr;
    if (context == NULL || button != DEVICE_BUTTON_RIGHT)
    {
        return;
    }
    if (event == DEVICE_BUTTON_EVENT_PRESS)
    {
        taskENTER_CRITICAL(&context->lock);
        context->right_press_started_at_us = esp_timer_get_time();
        taskEXIT_CRITICAL(&context->lock);
        return;
    }
    if (event == DEVICE_BUTTON_EVENT_RELEASE)
    {
        taskENTER_CRITICAL(&context->lock);
        context->right_press_started_at_us = 0;
        taskEXIT_CRITICAL(&context->lock);
        return;
    }
    if (event != DEVICE_BUTTON_EVENT_LONG_PRESS_END)
    {
        return;
    }

    const int64_t released_at_us = esp_timer_get_time();
    bool          request_setup  = false;
    taskENTER_CRITICAL(&context->lock);
    request_setup = utils_duration_reached_us(context->right_press_started_at_us,
                                              released_at_us,
                                              PROVISIONING_APP_SETUP_HOLD_US);
    context->right_press_started_at_us = 0;
    context->setup_requested = context->setup_requested || request_setup;
    taskEXIT_CRITICAL(&context->lock);
    if (request_setup)
    {
        xTaskNotifyGive(context->waiting_task);
    }
}

/** @brief 原子消费一次中键配网请求 */
static bool provisioning_app_take_setup_request(provisioning_app_button_context_t *context)
{
    bool requested;
    taskENTER_CRITICAL(&context->lock);
    requested = context->setup_requested;
    context->setup_requested = false;
    taskEXIT_CRITICAL(&context->lock);
    return requested;
}

/** @brief 居中显示两行英文状态并在刷新前保存正常页面恢复标记 */
static esp_err_t provisioning_app_show_prompt(const char *title)
{
    static const char *instruction = "HOLD MIDDLE 3S TO SETUP";
    device_display_info_t display_info = { 0 };
    device_display_ascii_size_t title_size = { 0 };
    device_display_ascii_size_t instruction_size = { 0 };
    ESP_RETURN_ON_ERROR(device_display_get_info_copy(&display_info), TAG, "读取网络提示显示信息失败");
    ESP_RETURN_ON_ERROR(device_display_measure_ascii_copy(title, 4U, &title_size),
                        TAG,
                        "计算网络提示标题尺寸失败");
    ESP_RETURN_ON_ERROR(device_display_measure_ascii_copy(instruction, 2U, &instruction_size),
                        TAG,
                        "计算网络提示说明尺寸失败");
    const uint16_t total_height = (uint16_t) (title_size.height_pixels
                                              + PROVISIONING_APP_PROMPT_GAP_PIXELS
                                              + instruction_size.height_pixels);
    ESP_RETURN_ON_FALSE(title_size.width_pixels <= display_info.width_pixels
                            && instruction_size.width_pixels <= display_info.width_pixels
                            && total_height <= display_info.height_pixels,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "网络提示文本超出显示区域");
    const uint16_t title_x = (uint16_t) ((display_info.width_pixels - title_size.width_pixels) / 2U);
    const uint16_t title_y = (uint16_t) ((display_info.height_pixels - total_height) / 2U);
    const uint16_t instruction_x =
        (uint16_t) ((display_info.width_pixels - instruction_size.width_pixels) / 2U);
    const uint16_t instruction_y =
        (uint16_t) (title_y + title_size.height_pixels + PROVISIONING_APP_PROMPT_GAP_PIXELS);
    ESP_RETURN_ON_ERROR(system_storage_set_display_restore_pending(true),
                        TAG,
                        "保存网络提示画面恢复标记失败");
    ESP_RETURN_ON_ERROR(device_display_clear(DEVICE_DISPLAY_TONE_WHITE), TAG, "清空网络提示内部帧失败");
    ESP_RETURN_ON_ERROR(device_display_draw_ascii_borrow(title_x, title_y, title, 4U),
                        TAG,
                        "绘制网络提示标题失败");
    ESP_RETURN_ON_ERROR(device_display_draw_ascii_borrow(instruction_x,
                                                         instruction_y,
                                                         instruction,
                                                         2U),
                        TAG,
                        "绘制网络提示说明失败");
    ESP_RETURN_ON_ERROR(device_display_present(), TAG, "刷新网络提示页失败");
    return ESP_OK;
}

/**
 * @brief 将当前 Portal 二维码居中渲染并一次性刷新到墨水屏
 *
 * @param[in] portal 已启动的 Portal 信息
 * @return ESP_OK 二维码已显示；其他值为状态、二维码或显示错误码
 */
static esp_err_t provisioning_app_show_portal(const connect_portal_info_t *portal)
{
    ESP_RETURN_ON_FALSE(portal != NULL, ESP_ERR_INVALID_ARG, TAG, "配网 Portal 信息为空");

    provisioning_app_qr_context_t context = { 0 };
    ESP_RETURN_ON_ERROR(device_display_get_info_copy(&context.display_info),
                        TAG,
                        "读取配网页面显示信息失败");

    const uint16_t max_side_pixels =
        context.display_info.width_pixels < context.display_info.height_pixels
            ? context.display_info.width_pixels
            : context.display_info.height_pixels;
    const connect_qr_sink_t sink = {
        .begin          = provisioning_app_qr_begin,
        .fill_dark_rect = provisioning_app_qr_fill_dark_rect,
    };
    ESP_RETURN_ON_ERROR(system_storage_set_display_restore_pending(true),
                        TAG,
                        "保存配网二维码画面恢复标记失败");
    ESP_RETURN_ON_ERROR(connect_render_portal_qr_borrow(portal, max_side_pixels, &sink, &context),
                        TAG,
                        "生成配网二维码失败");
    ESP_LOGI(TAG, "开始刷新墨水屏配网二维码");
    ESP_RETURN_ON_ERROR(device_display_present(), TAG, "刷新墨水屏配网二维码失败");
    return ESP_OK;
}

/**
 * @brief 清除已经显示的配网二维码，交出正常页面显示入口
 *
 * @return ESP_OK 白色页面已刷新；其他值为显示错误码
 */
static esp_err_t provisioning_app_clear_portal_page(void)
{
    ESP_RETURN_ON_ERROR(device_display_clear(DEVICE_DISPLAY_TONE_WHITE),
                        TAG,
                        "清空配网页面内部帧失败");
    ESP_LOGI(TAG, "网络已可用，开始清除配网二维码");
    ESP_RETURN_ON_ERROR(device_display_present(), TAG, "清除墨水屏配网二维码失败");
    return ESP_OK;
}

/** @brief 停止启动期按键扫描并结束回调借用 */
static esp_err_t provisioning_app_release_button_input(void)
{
    esp_err_t first_error = button_service_stop();
    if (first_error == ESP_ERR_INVALID_STATE)
    {
        first_error = ESP_OK;
    }
    const esp_err_t callback_error =
        button_service_set_event_callback_borrow(NULL, NULL);
    if (first_error == ESP_OK)
    {
        first_error = callback_error;
    }
    return first_error;
}

/**
 * @brief 结束变化通知回调借用，并在失败路径停止本轮网络会话
 *
 * @param[in] result 用例结果
 * @param[in] network_started 是否已经成功启动网络会话
 * @return 原用例错误；原用例成功时返回解除回调借用的错误
 */
static esp_err_t provisioning_app_finish(esp_err_t result, bool network_started)
{
    const esp_err_t button_error = provisioning_app_release_button_input();
    if (result == ESP_OK && button_error != ESP_OK)
    {
        result = button_error;
    }
    const esp_err_t callback_error = network_manager_set_notify_callback_borrow(NULL, NULL);
    if (result == ESP_OK && callback_error != ESP_OK)
    {
        result = callback_error;
    }
    if (result != ESP_OK && network_started)
    {
        const esp_err_t stop_error = network_manager_stop();
        if (stop_error != ESP_OK)
        {
            ESP_LOGE(TAG, "配网用例失败后停止网络会话失败: %s", esp_err_to_name(stop_error));
        }
    }
    return result;
}

/** @brief 使用有符号差值安全判断短期 Tick 截止时间是否已到 */
static bool provisioning_app_deadline_reached(TickType_t now, TickType_t deadline)
{
    return (int32_t) (now - deadline) >= 0;
}

/**
 * @brief 解除网络变化通知回调并把早期启动失败同步收敛到深睡
 *
 * @param[in] reason 触发深睡的原始错误
 * @param[in] policy 深睡唤醒策略
 * @return 仅在深睡准备失败时返回对应错误码
 */
static esp_err_t
    provisioning_app_finish_and_enter_sleep(esp_err_t                                   reason,
                                            power_management_app_startup_sleep_policy_t policy)
{
    const esp_err_t button_error = provisioning_app_release_button_input();
    if (button_error != ESP_OK)
    {
        ESP_LOGE(TAG, "启动阶段深睡前停止按键扫描失败: %s", esp_err_to_name(button_error));
        reason = button_error;
    }
    const esp_err_t callback_error = network_manager_set_notify_callback_borrow(NULL, NULL);
    if (callback_error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "启动阶段深睡前解除网络变化通知回调失败: %s",
                 esp_err_to_name(callback_error));
    }
    const esp_err_t sleep_error = power_management_app_enter_startup_sleep(policy, reason);
    ESP_LOGE(TAG, "启动阶段深睡准备失败: %s", esp_err_to_name(sleep_error));
    return sleep_error;
}

esp_err_t provisioning_app_run_until_online(const provisioning_app_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->backend_ready_callback != NULL
                            && !(config->woken_by_button && config->woken_by_timer),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "配网用例启动配置无效");

    const TaskHandle_t waiting_task = xTaskGetCurrentTaskHandle();
    provisioning_app_button_context_t button_context = {
        .waiting_task = waiting_task,
        .lock = portMUX_INITIALIZER_UNLOCKED,
    };
    ESP_RETURN_ON_ERROR(button_service_set_event_callback_borrow(
                            provisioning_app_on_button_event, &button_context),
                        TAG,
                        "注册启动配网按键回调失败");
    esp_err_t error = button_service_start();
    if (error != ESP_OK)
    {
        (void) button_service_set_event_callback_borrow(NULL, NULL);
        return error;
    }
    error = network_manager_set_notify_callback_borrow(provisioning_app_on_network_change,
                                                       (void *) waiting_task);
    if (error != ESP_OK)
    {
        (void) provisioning_app_release_button_input();
        return error;
    }

    error = network_manager_start();
    if (error != ESP_OK)
    {
        return provisioning_app_finish(error, false);
    }

    bool       portal_requested              = config->force_portal;
    bool       portal_visible                = false;
    bool       portal_deadline_active        = false;
    bool       portal_start_deadline_active  = false;
    bool       prompt_deadline_active        = false;
    bool       prompt_waits_for_network      = false;
    TickType_t portal_deadline               = 0U;
    TickType_t prompt_deadline               = 0U;
    TickType_t portal_start_deadline         = 0U;
    uint32_t   last_portal_activity_sequence = 0U;
    esp_err_t  prompt_reason                 = ESP_FAIL;
    power_management_app_startup_sleep_policy_t prompt_sleep_policy =
        POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF;
    if (portal_requested)
    {
        error = network_manager_request_start_portal();
        if (error != ESP_OK)
        {
            return provisioning_app_finish(error, true);
        }
        portal_start_deadline = xTaskGetTickCount()
                                + pdMS_TO_TICKS(PROVISIONING_APP_PORTAL_START_MS);
        portal_start_deadline_active = true;
        ESP_LOGI(TAG, "检测到保留的配网意图，已请求恢复现有 Portal");
    }
    while (true)
    {
        /* 丢弃读取状态前的旧计数；读取后的变化仍会保留通知，避免丢失唤醒。 */
        (void) ulTaskNotifyTake(pdTRUE, 0U);

        network_manager_status_t status;
        error = network_manager_get_status_copy(&status);
        if (error != ESP_OK)
        {
            return provisioning_app_finish(error, true);
        }

        if (provisioning_app_take_setup_request(&button_context))
        {
            error = system_storage_set_provisioning_pending(true);
            if (error != ESP_OK)
            {
                return provisioning_app_finish(error, true);
            }
            error = network_manager_request_start_portal();
            if (error != ESP_OK)
            {
                return provisioning_app_finish(error, true);
            }
            portal_requested       = true;
            portal_visible         = false;
            prompt_deadline_active = false;
            portal_start_deadline = xTaskGetTickCount()
                                    + pdMS_TO_TICKS(PROVISIONING_APP_PORTAL_START_MS);
            portal_start_deadline_active = true;
            ESP_LOGI(TAG, "物理中键已持续三秒，已请求进入现有配网 Portal");
        }

        switch (status.state)
        {
            case NETWORK_STATE_ONLINE:
                if (portal_requested && !portal_visible)
                {
                    /* START_PORTAL 排在 BOOT 后，旧配置可能短暂上线；必须等待 Portal 真正进入。 */
                    break;
                }
                error = config->backend_ready_callback(config->backend_ready_context);
                if (error == ESP_ERR_NOT_FOUND)
                {
                    if (!prompt_deadline_active)
                    {
                        const esp_err_t prompt_error = provisioning_app_show_prompt("NO SERVER");
                        if (prompt_error != ESP_OK)
                        {
                            return provisioning_app_finish(prompt_error, true);
                        }
                        prompt_reason       = error;
                        prompt_waits_for_network = false;
                        prompt_sleep_policy = POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF;
                        if (config->woken_by_timer)
                        {
                            return provisioning_app_finish_and_enter_sleep(prompt_reason,
                                                                           prompt_sleep_policy);
                        }
                        prompt_deadline = xTaskGetTickCount()
                                          + pdMS_TO_TICKS(PROVISIONING_APP_PROMPT_IDLE_MS);
                        prompt_deadline_active = true;
                    }
                    break;
                }
                if (error != ESP_OK)
                {
                    return provisioning_app_finish(error, true);
                }
                if (portal_visible)
                {
                    error = system_storage_set_provisioning_pending(false);
                    if (error != ESP_OK)
                    {
                        return provisioning_app_finish(error, true);
                    }
                    error = provisioning_app_clear_portal_page();
                    if (error != ESP_OK)
                    {
                        return provisioning_app_finish(error, true);
                    }
                }
                ESP_LOGI(TAG, "网络已获得可用 IPv4，进入正常页面流程");
                return provisioning_app_finish(ESP_OK, true);

            case NETWORK_STATE_PROVISIONING:
            case NETWORK_STATE_VALIDATING:
                if (!portal_visible)
                {
                    connect_portal_info_t portal_info;
                    error = network_manager_get_portal_info_copy(&portal_info);
                    if (error != ESP_OK)
                    {
                        return provisioning_app_finish(error, true);
                    }
                    if (!portal_info.active)
                    {
                        break;
                    }
                    error = provisioning_app_show_portal(&portal_info);
                    if (error != ESP_OK)
                    {
                        return provisioning_app_finish(error, true);
                    }
                    portal_visible = true;
                    portal_start_deadline_active = false;
                    prompt_deadline_active = false;
                    ESP_LOGI(TAG, "配网 Portal 已就绪，等待用户配置");
                }
                break;

            case NETWORK_STATE_ERROR: {
                error = status.last_error != ESP_OK ? status.last_error : ESP_FAIL;
                bool            has_saved_config = false;
                const esp_err_t config_status_error =
                    network_manager_has_saved_config(&has_saved_config);
                if (config_status_error != ESP_OK)
                {
                    return provisioning_app_finish(config_status_error, true);
                }
                if (portal_requested)
                {
                    break;
                }
                if (!prompt_deadline_active)
                {
                    const esp_err_t prompt_error = provisioning_app_show_prompt("NO NETWORK");
                    if (prompt_error != ESP_OK)
                    {
                        return provisioning_app_finish(prompt_error, true);
                    }
                    prompt_reason = error;
                    prompt_waits_for_network = true;
                    prompt_sleep_policy = has_saved_config
                                              ? POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF
                                              : POWER_MANAGEMENT_APP_STARTUP_SLEEP_UNTIL_BUTTON;
                    if (config->woken_by_timer)
                    {
                        return provisioning_app_finish_and_enter_sleep(prompt_reason,
                                                                       prompt_sleep_policy);
                    }
                    prompt_deadline = xTaskGetTickCount()
                                      + pdMS_TO_TICKS(PROVISIONING_APP_PROMPT_IDLE_MS);
                    prompt_deadline_active = true;
                    ESP_LOGW(TAG, "网络不可用，等待物理中键长按三秒进入配网");
                }
                break;
            }

            case NETWORK_STATE_STOPPED:
            case NETWORK_STATE_CONNECTING:
            case NETWORK_STATE_RETRY_WAIT:
            case NETWORK_STATE_STOPPING:
            default:
                break;
        }

        const bool portal_interactive =
            status.state == NETWORK_STATE_PROVISIONING || status.state == NETWORK_STATE_VALIDATING;
        if (!portal_interactive)
        {
            portal_deadline_active = false;
        }

        if (prompt_deadline_active
            && provisioning_app_deadline_reached(xTaskGetTickCount(), prompt_deadline))
        {
            network_manager_status_t latest_status;
            error = network_manager_get_status_copy(&latest_status);
            if (error != ESP_OK)
            {
                return provisioning_app_finish(error, true);
            }
            if (prompt_waits_for_network && latest_status.state == NETWORK_STATE_ONLINE)
            {
                prompt_deadline_active = false;
                continue;
            }
            ESP_LOGW(TAG, "网络提示页已等待三分钟，按既定策略进入深睡");
            return provisioning_app_finish_and_enter_sleep(prompt_reason, prompt_sleep_policy);
        }
        if (portal_start_deadline_active
            && provisioning_app_deadline_reached(xTaskGetTickCount(), portal_start_deadline))
        {
            ESP_LOGE(TAG, "等待现有配网 Portal 启动超时");
            return provisioning_app_finish_and_enter_sleep(
                ESP_ERR_TIMEOUT,
                POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF);
        }
        else if (portal_visible
                 && (!portal_deadline_active
                     || status.portal_activity_sequence != last_portal_activity_sequence))
        {
            portal_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PROVISIONING_APP_PORTAL_IDLE_MS);
            portal_deadline_active        = true;
            last_portal_activity_sequence = status.portal_activity_sequence;
            ESP_LOGI(TAG,
                     "配网活动窗口已更新，将等待 %lu 秒无交互后休眠",
                     (unsigned long) (PROVISIONING_APP_PORTAL_IDLE_MS / 1000U));
        }

        TickType_t wait_ticks = portMAX_DELAY;
        if (portal_deadline_active)
        {
            const TickType_t now = xTaskGetTickCount();
            wait_ticks           = provisioning_app_deadline_reached(now, portal_deadline)
                                       ? 0U
                                       : portal_deadline - now;
        }
        if (prompt_deadline_active)
        {
            const TickType_t now = xTaskGetTickCount();
            const TickType_t prompt_wait = provisioning_app_deadline_reached(now, prompt_deadline)
                                               ? 0U
                                               : prompt_deadline - now;
            if (wait_ticks == portMAX_DELAY || prompt_wait < wait_ticks)
            {
                wait_ticks = prompt_wait;
            }
        }
        if (portal_start_deadline_active)
        {
            const TickType_t now = xTaskGetTickCount();
            const TickType_t portal_start_wait =
                provisioning_app_deadline_reached(now, portal_start_deadline)
                    ? 0U
                    : portal_start_deadline - now;
            if (wait_ticks == portMAX_DELAY || portal_start_wait < wait_ticks)
            {
                wait_ticks = portal_start_wait;
            }
        }
        const uint32_t notification_count = ulTaskNotifyTake(pdTRUE, wait_ticks);
        if (notification_count == 0U && portal_deadline_active
            && provisioning_app_deadline_reached(xTaskGetTickCount(), portal_deadline))
        {
            network_manager_status_t latest_status;
            error = network_manager_get_status_copy(&latest_status);
            if (error != ESP_OK)
            {
                return provisioning_app_finish(error, true);
            }
            if (latest_status.state == NETWORK_STATE_ONLINE)
            {
                continue;
            }
            const bool still_interactive = latest_status.state == NETWORK_STATE_PROVISIONING
                                           || latest_status.state == NETWORK_STATE_VALIDATING;
            if (!still_interactive)
            {
                portal_deadline_active = false;
                continue;
            }
            if (latest_status.portal_activity_sequence != last_portal_activity_sequence)
            {
                last_portal_activity_sequence = latest_status.portal_activity_sequence;
                portal_deadline =
                    xTaskGetTickCount() + pdMS_TO_TICKS(PROVISIONING_APP_PORTAL_IDLE_MS);
                ESP_LOGI(TAG, "截止时检测到新的配网活动，重新计算三分钟窗口");
                continue;
            }
            ESP_LOGW(TAG,
                     "配网 Portal 已连续 %lu 秒无交互，保留二维码并进入深睡",
                     (unsigned long) (PROVISIONING_APP_PORTAL_IDLE_MS / 1000U));
            return provisioning_app_finish_and_enter_sleep(
                ESP_ERR_TIMEOUT,
                POWER_MANAGEMENT_APP_STARTUP_SLEEP_UNTIL_BUTTON);
        }
    }
}
