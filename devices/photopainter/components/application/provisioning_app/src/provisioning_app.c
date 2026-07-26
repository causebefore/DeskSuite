/**
 * @file provisioning_app.c
 * @brief 实现网络状态、配网页面和启动失败深睡收敛的同步 Application 用例
 */

#include "provisioning_app.h"

#include <stdbool.h>
#include <stdint.h>

#include "connect.h"
#include "device_display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "power_management_app.h"

/** @brief 日志标签 */
static const char *TAG = "provisioning_app";
/** @brief Portal 无显式用户活动时的清醒等待时间 */
#define PROVISIONING_APP_PORTAL_IDLE_MS 180000U

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

/**
 * @brief 结束变化通知回调借用，并在失败路径停止本轮网络会话
 *
 * @param[in] result 用例结果
 * @param[in] network_started 是否已经成功启动网络会话
 * @return 原用例错误；原用例成功时返回解除回调借用的错误
 */
static esp_err_t provisioning_app_finish(esp_err_t result, bool network_started)
{
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
    ESP_RETURN_ON_FALSE(config != NULL && !(config->woken_by_button && config->woken_by_timer),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "配网用例启动配置无效");

    const TaskHandle_t waiting_task = xTaskGetCurrentTaskHandle();
    ESP_RETURN_ON_ERROR(
        network_manager_set_notify_callback_borrow(provisioning_app_on_network_change,
                                                   (void *) waiting_task),
        TAG,
        "注册网络状态变化通知失败");

    esp_err_t error = network_manager_start();
    if (error != ESP_OK)
    {
        return provisioning_app_finish(error, false);
    }

    bool       portal_requested              = false;
    bool       portal_visible                = false;
    bool       portal_deadline_active        = false;
    TickType_t portal_deadline               = 0U;
    uint32_t   last_portal_activity_sequence = 0U;
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

        switch (status.state)
        {
            case NETWORK_STATE_ONLINE:
                if (portal_visible)
                {
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
                const bool no_config        = !has_saved_config && error == ESP_ERR_NOT_FOUND;
                const bool repair_requested = has_saved_config && config->woken_by_button;
                if ((no_config && !config->woken_by_timer) || repair_requested)
                {
                    if (portal_requested)
                    {
                        return provisioning_app_finish_and_enter_sleep(
                            error,
                            POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF);
                    }
                    error = network_manager_request_start_portal();
                    if (error != ESP_OK)
                    {
                        return provisioning_app_finish_and_enter_sleep(
                            error,
                            POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF);
                    }
                    portal_requested = true;
                    ESP_LOGI(TAG,
                             "%s，已请求进入三分钟无交互配网窗口",
                             no_config ? "未找到有效网络配置" : "检测到用户维修意图");
                    break;
                }
                if (no_config)
                {
                    ESP_LOGW(TAG, "定时唤醒时没有网络配置，等待按键唤醒后再配网");
                    return provisioning_app_finish_and_enter_sleep(
                        error,
                        POWER_MANAGEMENT_APP_STARTUP_SLEEP_UNTIL_BUTTON);
                }
                ESP_LOGW(TAG, "网络启动失败，准备按退避计划休眠: error=%s", esp_err_to_name(error));
                return provisioning_app_finish_and_enter_sleep(
                    error,
                    POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF);
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
