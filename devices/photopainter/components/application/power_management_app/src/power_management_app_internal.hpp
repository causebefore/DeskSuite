/**
 * @file power_management_app_internal.hpp
 * @brief 电源管理 App 私有 Runtime、通知位和 Task 接口
 */
#pragma once

#include "content_refresh_app.h"
#include "firmware_ota.h"
#include "power_management_app.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** @brief 电源管理 Task 栈大小 */
static constexpr uint32_t POWER_MANAGEMENT_TASK_STACK_SIZE = 6144U;
/** @brief 电源管理 Task 优先级 */
static constexpr UBaseType_t POWER_MANAGEMENT_TASK_PRIORITY = 3U;
/** @brief 空闲 Task 停止最长等待时间 */
static constexpr uint32_t POWER_MANAGEMENT_STOP_TIMEOUT_MS = 2000U;
/** @brief 进入深睡请求通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_SLEEP = 1UL << 0U;
/** @brief 停止 Task 请求通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_STOP = 1UL << 1U;
/** @brief 内容刷新轮次完成通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_REFRESH_ROUND = 1UL << 2U;
/** @brief 照片集合显示收敛通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_DISPLAY_SETTLED = 1UL << 3U;
/** @brief 用户完成左右导航通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_USER_ACTIVITY = 1UL << 4U;
/** @brief 内容刷新 Task 启动失败通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_REFRESH_START_FAILURE = 1UL << 5U;
/** @brief 确认键手动完整内容刷新已经提交通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_MANUAL_REFRESH = 1UL << 7U;
/** @brief 左键三秒长按一次性固件检查请求通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_FIRMWARE_CHECK = 1UL << 6U;
/** @brief 模态状态页左键取消或返回通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_MODAL_LEFT = 1UL << 8U;
/** @brief 模态状态页确认安装通知位 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_MODAL_CONFIRM = 1UL << 9U;
/** @brief OTA Task 已复制一份不可变事务完成事件 */
static constexpr uint32_t POWER_MANAGEMENT_NOTIFY_FIRMWARE_OTA_EVENT = 1UL << 10U;

/** @brief 电源管理 App 进程期唯一 Runtime */
class PowerManagementRuntime final
{
public:
    PowerManagementRuntime() = default;
    PowerManagementRuntime(const PowerManagementRuntime &) = delete;
    PowerManagementRuntime &operator=(const PowerManagementRuntime &) = delete;
    PowerManagementRuntime(PowerManagementRuntime &&) = delete;
    PowerManagementRuntime &operator=(PowerManagementRuntime &&) = delete;

    bool initialized = false; /**< 生命周期初始化标记 */
    SemaphoreHandle_t task_stopped = nullptr; /**< Task 退出握手 */
    SemaphoreHandle_t network_changed = nullptr; /**< OTA 会话网络状态变化信号 */
    TaskHandle_t task = nullptr; /**< 电源管理 Task */
    portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED; /**< 状态短临界区 */
    uint32_t interactive_awake_ms = 0U; /**< 按键启动后的无活动窗口 */
    bool round_event_pending = false; /**< 是否有未消费的最新轮次事件 */
    content_refresh_app_round_event_t latest_round_event = {}; /**< 最新轮次事件副本 */
    bool firmware_ota_event_pending = false; /**< 是否有未消费的 OTA 完成事件 */
    firmware_ota_event_t latest_firmware_ota_event = {}; /**< 最新 OTA 完成事件副本 */
    esp_err_t refresh_start_error = ESP_OK; /**< 内容刷新 Task 启动失败事实 */
    power_management_app_status_t status = {}; /**< 对外状态 */
};

/** @brief 电源管理 App 唯一 Runtime */
extern PowerManagementRuntime g_power_management_runtime;

/** @brief 发布状态和最近错误 */
void power_management_app_publish(power_management_app_state_t state, esp_err_t error);

/** @brief 启用由本 App 回调接收的 OTA 模态按键捕获 */
esp_err_t power_management_app_begin_ota_modal_capture(void);

/** @brief Network Manager 状态变化回调，仅释放 OTA 等待信号量 */
void power_management_app_on_network_change(void *context);

/** @brief 创建电源管理 Task */
esp_err_t power_management_app_task_start(void);

/** @brief 同步停止空闲的电源管理 Task */
esp_err_t power_management_app_task_stop(void);

/**
 * @brief 根据唤醒来源和 RTC 保留目标执行启动绝对时间门禁
 *
 * @param[in] woken_by_button 本次是否由任意按键唤醒
 * @param[in] woken_by_timer 本次是否由内部定时器唤醒
 * @return ESP_OK 门禁允许继续启动；或读取可信时间、重新准备深睡错误码
 */
esp_err_t power_management_app_apply_startup_time_gate(bool woken_by_button,
                                                       bool woken_by_timer);

/**
 * @brief 按已校验的启动阶段策略同步执行有序停机和深睡
 *
 * @param[in] policy 已由公共入口校验的启动深睡策略
 * @param[in] reason 触发深睡的原始错误
 * @return 仅在未进入深睡时返回停机、唤醒源、显示或回滚错误码
 */
esp_err_t power_management_app_prepare_startup_sleep(
    power_management_app_startup_sleep_policy_t policy, esp_err_t reason);
