/**
 * @file app_power_task.c
 * @brief 以单一 Application Task 编排离线显示与可配置维护源 Light-sleep
 */
#include "app_power.h"

#include <stdbool.h>
#include <time.h>

#include "app_network.h"
#include "app_pomodoro.h"
#include "app_voice.h"
#include "button_service.h"
#include "device_power.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "system_clock.h"
#include "ui_runtime.h"

#define APP_POWER_TASK_STACK_SIZE               6144U
#define APP_POWER_TASK_PRIORITY                 3U
#define APP_POWER_VOICE_LIFECYCLE_TIMEOUT_MS    3000U
#define APP_POWER_UI_LIFECYCLE_TIMEOUT_MS       5000U
#define APP_POWER_NETWORK_LIFECYCLE_TIMEOUT_MS  5000U
#define APP_POWER_NETWORK_SYNC_CLAIM_TIMEOUT_MS 5000U
#define APP_POWER_POMODORO_RECONCILE_TIMEOUT_MS 1000U

static const char *TAG                  = "app_power";

static portMUX_TYPE              s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_power_config_t        s_config;
static SemaphoreHandle_t         s_stopped_signal;
static TaskHandle_t              s_task;
static app_power_state_t         s_state;
static app_power_step_t          s_step;
static app_power_wakeup_source_t s_wakeup_source;
static int64_t                   s_last_activity_us;
static uint32_t                  s_activity_generation;
static uint32_t                  s_cycle_id;
static uint32_t                  s_success_count;
static uint32_t                  s_timer_refresh_count;
static uint32_t                  s_blockers;
static esp_err_t                 s_primary_error;
static esp_err_t                 s_recovery_error;
static bool                      s_initialized;
static bool                      s_started;
static bool                      s_stop_requested;

/** @brief 在短临界区内更新主状态、步骤和错误事实 */
static void set_status(app_power_state_t state, app_power_step_t step, esp_err_t primary_error,
                       esp_err_t recovery_error)
{
    taskENTER_CRITICAL(&s_lock);
    s_state          = state;
    s_step           = step;
    s_primary_error  = primary_error;
    s_recovery_error = recovery_error;
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief 仅更新主状态和步骤，保留已经锁存的错误事实 */
static void set_state_step(app_power_state_t state, app_power_step_t step)
{
    taskENTER_CRITICAL(&s_lock);
    s_state = state;
    s_step  = step;
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief 读取停止请求 */
static bool stop_is_requested(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool requested = s_stop_requested;
    taskEXIT_CRITICAL(&s_lock);
    return requested;
}

/** @brief 判断准备期间是否出现新活动或停止请求 */
static bool preparation_was_interrupted(uint32_t expected_generation)
{
    taskENTER_CRITICAL(&s_lock);
    const bool interrupted = s_stop_requested || s_activity_generation != expected_generation;
    taskEXIT_CRITICAL(&s_lock);
    return interrupted;
}

/** @brief 保留首个恢复错误 */
static void keep_recovery_error(esp_err_t error)
{
    if (error == ESP_OK)
    {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_recovery_error == ESP_OK)
    {
        s_recovery_error = error;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief 保留首个主流程错误 */
static void keep_primary_error(esp_err_t error)
{
    if (error == ESP_OK)
    {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_primary_error == ESP_OK)
    {
        s_primary_error = error;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief 把 Device 唤醒事实转换为 Application 枚举，按键优先于同时命中的维护源 */
static app_power_wakeup_source_t map_wakeup_source(const device_power_wakeup_result_t *wakeup)
{
    if (wakeup->left_button && wakeup->right_button)
    {
        return APP_POWER_WAKEUP_BOTH_BUTTONS;
    }
    if (wakeup->left_button)
    {
        return APP_POWER_WAKEUP_LEFT_BUTTON;
    }
    if (wakeup->right_button)
    {
        return APP_POWER_WAKEUP_RIGHT_BUTTON;
    }
    if (wakeup->timer)
    {
        return APP_POWER_WAKEUP_TIMER;
    }
    return APP_POWER_WAKEUP_UNKNOWN;
}

/** @brief 判断锁存的唤醒来源是否要求恢复正常交互窗口 */
static bool wakeup_is_button(app_power_wakeup_source_t source)
{
    return source == APP_POWER_WAKEUP_LEFT_BUTTON || source == APP_POWER_WAKEUP_RIGHT_BUTTON
           || source == APP_POWER_WAKEUP_BOTH_BUTTONS;
}

/** @brief 读取不会改变其他组件状态的产品睡眠阻止条件 */
static uint32_t collect_runtime_blockers(void)
{
    uint32_t           blockers = APP_POWER_BLOCKER_NONE;
    app_voice_status_t voice    = { 0 };
    if (app_voice_get_status_copy(&voice) != ESP_OK || voice.state == APP_VOICE_STATE_FAILED || voice.session_busy)
    {
        blockers |= APP_POWER_BLOCKER_VOICE;
    }
    if (voice.input_active || voice.output_active)
    {
        blockers |= APP_POWER_BLOCKER_AUDIO;
    }
    if (!voice.processor_idle)
    {
        blockers |= APP_POWER_BLOCKER_AUDIO_PROCESSOR;
    }
    if (voice.network_lease_held)
    {
        blockers |= APP_POWER_BLOCKER_NETWORK_LEASE;
    }
    if (app_network_is_ota_busy())
    {
        blockers |= APP_POWER_BLOCKER_OTA;
    }

    app_network_lease_snapshot_t lease = { 0 };
    app_network_get_lease_snapshot_copy(&lease);
    if (lease.active)
    {
        blockers |= APP_POWER_BLOCKER_NETWORK_LEASE;
    }
    return blockers;
}

/** @brief 更新当前只读产品阻止原因 */
static void set_blockers(uint32_t blockers)
{
    taskENTER_CRITICAL(&s_lock);
    s_blockers = blockers;
    taskEXIT_CRITICAL(&s_lock);
}

/** @brief 等待通知或给定毫秒数，通知用于活动和停止请求 */
static void wait_notification(uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (ticks == 0)
    {
        ticks = 1;
    }
    (void) ulTaskNotifyTake(pdTRUE, ticks);
}

/** @brief 等待本轮无活动窗口结束 */
static void wait_awake_window(void)
{
    for (;;)
    {
        if (stop_is_requested())
        {
            return;
        }

        taskENTER_CRITICAL(&s_lock);
        const int64_t deadline_us = s_last_activity_us + (int64_t) s_config.idle_timeout_ms * 1000LL;
        taskEXIT_CRITICAL(&s_lock);

        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0)
        {
            return;
        }

        uint64_t remaining_ms = ((uint64_t) remaining_us + 999ULL) / 1000ULL;
        if (remaining_ms > UINT32_MAX)
        {
            remaining_ms = UINT32_MAX;
        }
        wait_notification((uint32_t) remaining_ms);
    }
}

/** @brief 开始一次新的物理睡眠尝试并返回本轮活动代次 */
static uint32_t begin_cycle(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_cycle_id++;
    s_state                   = APP_POWER_STATE_PREPARING;
    s_step                    = APP_POWER_STEP_CHECK_BLOCKERS;
    s_wakeup_source           = APP_POWER_WAKEUP_NONE;
    s_blockers                = APP_POWER_BLOCKER_NONE;
    s_primary_error           = ESP_OK;
    s_recovery_error          = ESP_OK;
    const uint32_t generation = s_activity_generation;
    taskEXIT_CRITICAL(&s_lock);
    return generation;
}

/** @brief 开始前台离线显示尝试，不增加物理 Light-sleep 计数 */
static uint32_t begin_offline_display_attempt(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_state                   = APP_POWER_STATE_PREPARING;
    s_step                    = APP_POWER_STEP_CHECK_BLOCKERS;
    s_wakeup_source           = APP_POWER_WAKEUP_NONE;
    s_blockers                = APP_POWER_BLOCKER_NONE;
    s_primary_error           = ESP_OK;
    s_recovery_error          = ESP_OK;
    const uint32_t generation = s_activity_generation;
    taskEXIT_CRITICAL(&s_lock);
    return generation;
}

/** @brief 把不可恢复错误锁存为 BLOCKED */
static void enter_blocked(esp_err_t primary_error, esp_err_t recovery_error)
{
    taskENTER_CRITICAL(&s_lock);
    const app_power_step_t step = s_step;
    taskEXIT_CRITICAL(&s_lock);
    set_status(APP_POWER_STATE_BLOCKED, step, primary_error, recovery_error);
    ESP_LOGE(TAG,
             "低功耗流程已阻止，主错误=%s，恢复错误=%s",
             esp_err_to_name(primary_error),
             esp_err_to_name(recovery_error));
}

/**
 * @brief 在活动代次未变化时停止语音 Runtime
 *
 * 活动会话、AFE drain、音频输入输出或未释放租约会由 app_voice 拒绝停止；本函数不取消会话。
 *
 * @param[in] expected_generation 计划入睡时锁存的活动代次
 * @return ESP_OK 语音链已静默；ESP_ERR_INVALID_STATE 本轮准备被取消或语音仍忙；
 *         其他值表示语音停止或回滚失败
 */
static esp_err_t stop_voice_for_sleep(uint32_t expected_generation)
{
    if (preparation_was_interrupted(expected_generation))
    {
        return ESP_ERR_INVALID_STATE;
    }

    set_state_step(APP_POWER_STATE_PREPARING, APP_POWER_STEP_VOICE_STOP);
    const esp_err_t error = app_voice_stop(APP_POWER_VOICE_LIFECYCLE_TIMEOUT_MS);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        keep_primary_error(error);
    }
    return error;
}

/**
 * @brief 恢复语音 Runtime 和按键语音入口
 *
 * @return ESP_OK 语音链已恢复；其他值表示恢复失败
 */
static esp_err_t resume_voice_runtime(void)
{
    set_state_step(APP_POWER_STATE_RESUMING, APP_POWER_STEP_VOICE_START);
    const esp_err_t error = app_voice_start(APP_POWER_VOICE_LIFECYCLE_TIMEOUT_MS);
    keep_recovery_error(error);
    return error;
}

/**
 * @brief 恢复已经成功停止的 UI，并保留恢复阶段首个错误
 *
 * 返回 ESP_OK 时，UI Runtime 已把 Presenter 最新状态同步到控件树并完成一次显示传输。
 *
 * @return ESP_OK UI 已恢复且画面已刷新；其他值为 UI 恢复错误
 */
static esp_err_t resume_ui_runtime(void)
{
    set_state_step(APP_POWER_STATE_RESUMING, APP_POWER_STEP_UI_START);
    const esp_err_t error = ui_runtime_start(APP_POWER_UI_LIFECYCLE_TIMEOUT_MS);
    keep_recovery_error(error);
    return error;
}

/**
 * @brief 在活动代次未变化时可逆停止 UI
 *
 * @param[in] expected_generation 计划入睡时锁存的活动代次
 * @return ESP_OK UI 已停止；ESP_ERR_INVALID_STATE 准备被用户活动或停止请求取消；
 *         其他值表示 UI 停止失败
 */
static esp_err_t stop_ui_for_sleep(uint32_t expected_generation)
{
    if (preparation_was_interrupted(expected_generation))
    {
        return ESP_ERR_INVALID_STATE;
    }

    set_state_step(APP_POWER_STATE_PREPARING, APP_POWER_STEP_UI_STOP);
    const esp_err_t error = ui_runtime_stop(APP_POWER_UI_LIFECYCLE_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        keep_primary_error(error);
        return error == ESP_ERR_INVALID_STATE ? ESP_FAIL : error;
    }
    return ESP_OK;
}

/**
 * @brief 在活动代次未变化时同步停止网络策略与 Wi-Fi Driver
 *
 * @param[in] expected_generation 计划入睡时锁存的活动代次
 * @return ESP_OK 网络已暂停；ESP_ERR_INVALID_STATE 准备被活动取消或网络产品冲突；
 *         其他值表示网络停止失败
 */
static esp_err_t stop_network_for_power_save(uint32_t expected_generation)
{
    if (preparation_was_interrupted(expected_generation))
    {
        return ESP_ERR_INVALID_STATE;
    }

    set_state_step(APP_POWER_STATE_PREPARING, APP_POWER_STEP_NETWORK_STOP);
    const esp_err_t error = app_network_suspend_for_power_save(APP_POWER_NETWORK_LIFECYCLE_TIMEOUT_MS);
    keep_primary_error(error);
    return error;
}

/**
 * @brief 恢复低功耗停网前停止的网络策略
 *
 * @return ESP_OK 网络连接策略已重新启动；其他值表示恢复失败
 */
static esp_err_t resume_network_runtime(void)
{
    set_state_step(APP_POWER_STATE_RESUMING, APP_POWER_STEP_NETWORK_START);
    const esp_err_t error = app_network_resume_from_power_save(APP_POWER_NETWORK_LIFECYCLE_TIMEOUT_MS);
    keep_recovery_error(error);
    return error;
}

/**
 * @brief 按网络、语音、UI 的固定顺序恢复已停止的 Runtime
 *
 * @param[in,out] network_suspended 网络暂停状态；成功恢复后写 false
 * @param[in,out] voice_stopped 语音停止状态；成功恢复后写 false
 * @param[in,out] ui_stopped UI 停止状态；成功恢复后写 false
 * @return ESP_OK 三者均已恢复；其他值为首个恢复错误
 */
static esp_err_t restore_awake_runtime(bool *network_suspended, bool *voice_stopped, bool *ui_stopped)
{
    esp_err_t first_error = ESP_OK;
    if (*network_suspended)
    {
        const esp_err_t error = resume_network_runtime();
        if (error == ESP_OK)
        {
            *network_suspended = false;
        }
        else
        {
            first_error = error;
        }
    }
    if (*voice_stopped)
    {
        const esp_err_t error = resume_voice_runtime();
        if (error == ESP_OK)
        {
            *voice_stopped = false;
        }
        else if (first_error == ESP_OK)
        {
            first_error = error;
        }
    }
    if (*ui_stopped)
    {
        const esp_err_t error = resume_ui_runtime();
        if (error == ESP_OK)
        {
            *ui_stopped = false;
        }
        else if (first_error == ESP_OK)
        {
            first_error = error;
        }
    }
    return first_error;
}

/** @brief 判断当前 Dashboard 正常截止或失败退避截止是否已经到达；未知时间按到期处理 */
static bool network_maintenance_is_due(void)
{
    int64_t next_sync_at_utc = 0;
    if (app_network_get_next_dashboard_sync_at_utc(&next_sync_at_utc) != ESP_OK)
    {
        return true;
    }

    system_clock_snapshot_t clock = { 0 };
    if (system_clock_get_snapshot_copy(&clock) != ESP_OK || !clock.valid)
    {
        return true;
    }
    return (int64_t) clock.utc_timestamp >= next_sync_at_utc;
}

typedef enum
{
    APP_POWER_TIMER_REASON_SCREEN = 0,
    APP_POWER_TIMER_REASON_DASHBOARD,
    APP_POWER_TIMER_REASON_POMODORO,
} app_power_timer_reason_t;

/**
 * @brief 取屏幕维护、Dashboard 同步截止和番茄钟截止中更近的低功耗等待间隔
 *
 * @param[out] out_reason 可选的本轮间隔来源
 * @return 下一次低功耗维护间隔，单位毫秒，至少为 1
 */
static uint32_t next_power_save_interval_ms(app_power_timer_reason_t *out_reason)
{
    uint32_t                 interval_ms     = s_config.refresh_interval_ms;
    app_power_timer_reason_t reason          = APP_POWER_TIMER_REASON_SCREEN;

    int64_t                 next_sync_at_utc = 0;
    system_clock_snapshot_t clock            = { 0 };
    if (app_network_get_next_dashboard_sync_at_utc(&next_sync_at_utc) == ESP_OK
        && system_clock_get_snapshot_copy(&clock) == ESP_OK && clock.valid)
    {
        const int64_t  remaining_seconds = next_sync_at_utc - (int64_t) clock.utc_timestamp;
        const uint64_t remaining_ms      = remaining_seconds <= 0 ? 1ULL : (uint64_t) remaining_seconds * 1000ULL;
        if (remaining_ms < interval_ms)
        {
            interval_ms = (uint32_t) remaining_ms;
            reason      = APP_POWER_TIMER_REASON_DASHBOARD;
        }
    }

    uint32_t pomodoro_ms = 0U;
    if (app_pomodoro_get_next_wakeup_interval_ms(&pomodoro_ms) == ESP_OK && pomodoro_ms <= interval_ms)
    {
        interval_ms = pomodoro_ms;
        reason      = APP_POWER_TIMER_REASON_POMODORO;
    }
    if (out_reason != NULL)
    {
        *out_reason = reason;
    }
    return interval_ms == 0U ? 1U : interval_ms;
}

#ifndef CONFIG_DESKMATE_RTC_INT_WAKE_TEST_ENABLED
/**
 * @brief 在进入 Light-sleep 前输出内部 Timer 的计划唤醒时间
 *
 * 按键可能早于计划 Timer 唤醒；系统时间不可信时只输出相对等待时长。
 *
 * @param[in] interval_ms 本轮内部 Timer 间隔，单位毫秒
 * @param[in] reason 本轮间隔来源
 */
static void log_next_wakeup(uint32_t interval_ms, app_power_timer_reason_t reason)
{
    const char *reason_text =
        reason == APP_POWER_TIMER_REASON_POMODORO
            ? "番茄钟阶段截止"
            : (reason == APP_POWER_TIMER_REASON_DASHBOARD ? "Dashboard 同步截止" : "屏幕维护周期");
    system_clock_snapshot_t clock = { 0 };
    if (system_clock_get_snapshot_copy(&clock) == ESP_OK && clock.valid)
    {
        const time_t planned_utc = clock.utc_timestamp + (time_t) (((uint64_t) interval_ms + 999ULL) / 1000ULL);
        struct tm    planned_local;
        if (localtime_r(&planned_utc, &planned_local) != NULL)
        {
            ESP_LOGI(TAG,
                     "准备进入轻睡眠，Timer 计划唤醒=%04d-%02d-%02d %02d:%02d:%02d"
                     "（本地，UTC=%lld），等待=%lu ms，原因=%s；按键可提前唤醒",
                     planned_local.tm_year + 1900,
                     planned_local.tm_mon + 1,
                     planned_local.tm_mday,
                     planned_local.tm_hour,
                     planned_local.tm_min,
                     planned_local.tm_sec,
                     (long long) planned_utc,
                     (unsigned long) interval_ms,
                     reason_text);
            return;
        }
    }

    ESP_LOGI(TAG,
             "准备进入轻睡眠，Timer 计划在 %lu ms 后唤醒（系统时间不可信），原因=%s；按键可提前唤醒",
             (unsigned long) interval_ms,
             reason_text);
}
#endif

/**
 * @brief 在 Timer 唤醒维护窗口恢复网络、同步 Dashboard 并再次可逆停网
 *
 * Dashboard 同步失败不会破坏旧截止时间，网络成功停回后允许继续按分钟重试；网络 start/stop
 * 失败则无法证明整机睡眠条件，作为主流程错误返回。
 *
 * @param[in] expected_generation 当前睡眠会话锁存的活动代次
 * @param[in,out] network_suspended 网络暂停状态
 * @return ESP_OK 维护已完成或同步失败后已安全停网；ESP_ERR_INVALID_STATE 维护期间出现活动；
 *         其他值表示网络生命周期失败
 */
static esp_err_t run_network_maintenance(uint32_t expected_generation, bool *network_suspended)
{
    esp_err_t error = resume_network_runtime();
    if (error != ESP_OK)
    {
        return error;
    }
    *network_suspended = false;

    set_state_step(APP_POWER_STATE_RESUMING, APP_POWER_STEP_NETWORK_SYNC);
    const esp_err_t sync_error = app_network_sync_for_power_save(APP_POWER_NETWORK_SYNC_CLAIM_TIMEOUT_MS);
    if (sync_error == ESP_OK)
    {
        int64_t next_sync_at_utc = 0;
        if (app_network_get_next_dashboard_sync_at_utc(&next_sync_at_utc) == ESP_OK)
        {
            ESP_LOGI(TAG, "联网维护同步完成，下一次 Dashboard 同步 UTC=%lld", (long long) next_sync_at_utc);
        }
    }
    else
    {
        ESP_LOGW(TAG, "联网维护同步失败，将在下次 Timer 唤醒重试: %s", esp_err_to_name(sync_error));
    }

    if (preparation_was_interrupted(expected_generation))
    {
        return ESP_ERR_INVALID_STATE;
    }

    set_state_step(APP_POWER_STATE_PREPARING, APP_POWER_STEP_NETWORK_STOP);
    error = app_network_suspend_for_power_save(APP_POWER_NETWORK_LIFECYCLE_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        keep_primary_error(error);
        return error;
    }
    *network_suspended = true;
    return ESP_OK;
}

/**
 * @brief 保持番茄钟 UI 秒级运行，仅暂停网络并按 Dashboard 截止临时联网维护
 *
 * 用户活动、番茄钟离开 RUNNING 前台状态或停止请求都会先恢复网络再退出。该会话不停止语音
 * Runtime、不停止 UI，也不进入 Device Light-sleep；网络维护完成后在活动代次未变化时再次停网。
 *
 * @param[in] initial_generation 进入离线显示前锁存的活动代次
 * @return ESP_OK 已恢复正常清醒网络；ESP_ERR_INVALID_STATE 产品冲突或准备被活动取消；
 *         其他值表示网络停止或恢复失败
 */
static esp_err_t run_offline_display_session(uint32_t initial_generation)
{
    bool      network_suspended = false;
    esp_err_t result            = stop_network_for_power_save(initial_generation);
    if (result != ESP_OK)
    {
        return result;
    }
    network_suspended = true;
    set_status(APP_POWER_STATE_OFFLINE_DISPLAY, APP_POWER_STEP_NONE, ESP_OK, ESP_OK);
    ESP_LOGI(TAG, "番茄钟前台进入离线显示，Wi-Fi 已关闭且 UI 保持秒级刷新");

    for (;;)
    {
        wait_notification(next_power_save_interval_ms(NULL));
        if (preparation_was_interrupted(initial_generation) || !app_pomodoro_requires_live_display())
        {
            result = resume_network_runtime();
            if (result == ESP_OK)
            {
                network_suspended = false;
                set_status(APP_POWER_STATE_AWAKE, APP_POWER_STEP_NONE, ESP_OK, ESP_OK);
                ESP_LOGI(TAG, "番茄钟离线显示结束，网络连接策略已恢复");
            }
            return result;
        }

        if (!network_maintenance_is_due())
        {
            set_state_step(APP_POWER_STATE_OFFLINE_DISPLAY, APP_POWER_STEP_NONE);
            continue;
        }

        result = run_network_maintenance(initial_generation, &network_suspended);
        if (result != ESP_OK)
        {
            break;
        }
        set_status(APP_POWER_STATE_OFFLINE_DISPLAY, APP_POWER_STEP_NONE, ESP_OK, ESP_OK);
    }

    if (network_suspended)
    {
        const esp_err_t recovery_error = resume_network_runtime();
        if (recovery_error != ESP_OK)
        {
            return result == ESP_OK ? recovery_error : result;
        }
    }
    if (result == ESP_ERR_INVALID_STATE)
    {
        set_status(APP_POWER_STATE_AWAKE, APP_POWER_STEP_NONE, ESP_OK, ESP_OK);
    }
    return result;
}

/**
 * @brief 执行一次睡眠会话，Timer 到期时联网维护并刷新屏幕后继续睡眠
 *
 * 首次入睡前依次停止语音、UI 和网络。按键唤醒按网络、语音、UI 的顺序恢复后结束睡眠会话；Timer 唤醒仅在
 * 服务端截止时间到达时恢复网络并同步 Dashboard，随后再次停网、恢复 UI 完成一次画面刷新，
 * 期间语音保持停止；确认没有新活动和产品阻止条件后再次停止 UI 并进入下一轮睡眠。
 *
 * @param[in] initial_generation 首次开始准备时锁存的活动代次
 * @return ESP_OK 已返回正常清醒流程；ESP_ERR_INVALID_STATE 准备被活动取消或按键未释放；
 *         其他值表示睡眠或 UI 生命周期不可靠
 */
static esp_err_t run_sleep_session(uint32_t initial_generation)
{
    uint32_t  expected_generation = initial_generation;
    bool      voice_stopped       = false;
    bool      ui_stopped          = false;
    bool      network_suspended   = false;
    esp_err_t result              = stop_voice_for_sleep(expected_generation);
    if (result != ESP_OK)
    {
        return result;
    }
    voice_stopped = true;

    result        = stop_ui_for_sleep(expected_generation);
    if (result != ESP_OK)
    {
        goto restore_awake;
    }
    ui_stopped = true;

    result     = stop_network_for_power_save(expected_generation);
    if (result != ESP_OK)
    {
        goto restore_awake;
    }
    network_suspended = true;

    for (;;)
    {
        if (preparation_was_interrupted(expected_generation))
        {
            result = ESP_ERR_INVALID_STATE;
            goto restore_awake;
        }

        set_state_step(APP_POWER_STATE_SLEEPING, APP_POWER_STEP_DEVICE_SLEEP);
        device_power_wakeup_result_t wakeup = { 0 };
        app_power_timer_reason_t     timer_reason       = APP_POWER_TIMER_REASON_SCREEN;
        const uint32_t               wakeup_interval_ms = next_power_save_interval_ms(&timer_reason);
        log_next_wakeup(wakeup_interval_ms, timer_reason);
        const esp_err_t sleep_result = device_power_enter_light_sleep(wakeup_interval_ms, &wakeup);

        taskENTER_CRITICAL(&s_lock);
        s_state         = APP_POWER_STATE_RESUMING;
        s_step          = APP_POWER_STEP_DEVICE_WAKE;
        s_primary_error = sleep_result;
        if (sleep_result == ESP_OK)
        {
            s_wakeup_source = map_wakeup_source(&wakeup);
        }
        const app_power_wakeup_source_t wakeup_source = s_wakeup_source;
        taskEXIT_CRITICAL(&s_lock);

        if (sleep_result != ESP_OK)
        {
            result = sleep_result;
            goto restore_awake;
        }
        if (wakeup_source == APP_POWER_WAKEUP_UNKNOWN)
        {
            keep_primary_error(ESP_ERR_INVALID_RESPONSE);
            result = ESP_ERR_INVALID_RESPONSE;
            goto restore_awake;
        }

        app_pomodoro_wakeup_result_t pomodoro_result = APP_POMODORO_WAKEUP_NO_CHANGE;
        result = app_pomodoro_reconcile_after_wakeup(APP_POWER_POMODORO_RECONCILE_TIMEOUT_MS, &pomodoro_result);
        if (result != ESP_OK)
        {
            keep_primary_error(result);
            ESP_LOGE(TAG, "睡眠唤醒后补算番茄钟失败: %s", esp_err_to_name(result));
            goto restore_awake;
        }
        if (pomodoro_result == APP_POMODORO_WAKEUP_PHASE_COMPLETED)
        {
            const esp_err_t activity_error = app_power_notify_activity();
            if (activity_error != ESP_OK)
            {
                keep_primary_error(activity_error);
                result = activity_error;
                goto restore_awake;
            }
            ESP_LOGI(TAG, "睡眠期间番茄钟阶段已完成，恢复 60 秒正常清醒窗口");
        }

        if (wakeup_is_button(wakeup_source))
        {
            result = resume_network_runtime();
            if (result != ESP_OK)
            {
                goto restore_awake;
            }
            network_suspended = false;

            result            = resume_voice_runtime();
            if (result != ESP_OK)
            {
                goto restore_awake;
            }
            voice_stopped = false;

            result        = resume_ui_runtime();
            if (result != ESP_OK)
            {
                goto restore_awake;
            }
            ui_stopped                                           = false;

            const button_service_wakeup_snapshot_t button_wakeup = {
                .left_button  = wakeup.left_button,
                .right_button = wakeup.right_button,
            };
            const esp_err_t button_error = button_service_request_light_sleep_wakeup_copy(&button_wakeup);
            if (button_error != ESP_OK)
            {
                keep_recovery_error(button_error);
                return button_error;
            }

            taskENTER_CRITICAL(&s_lock);
            s_success_count++;
            s_last_activity_us           = esp_timer_get_time();
            s_blockers                   = APP_POWER_BLOCKER_NONE;
            s_primary_error              = ESP_OK;
            s_recovery_error             = ESP_OK;
            const uint32_t success_count = s_success_count;
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "按键唤醒已恢复交互，成功次数=%lu", (unsigned long) success_count);
            return ESP_OK;
        }

        if (preparation_was_interrupted(expected_generation))
        {
            result = ESP_ERR_INVALID_STATE;
            goto restore_awake;
        }

        if (network_maintenance_is_due())
        {
            result = run_network_maintenance(expected_generation, &network_suspended);
            if (result != ESP_OK)
            {
                goto restore_awake;
            }
        }

        result = resume_ui_runtime();
        if (result != ESP_OK)
        {
            goto restore_awake;
        }
        ui_stopped = false;

        taskENTER_CRITICAL(&s_lock);
        s_timer_refresh_count++;
        s_primary_error  = ESP_OK;
        s_recovery_error = ESP_OK;
        const uint32_t refresh_count = s_timer_refresh_count;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "内部 Timer 唤醒已刷新屏幕，累计=%lu", (unsigned long) refresh_count);

        if (preparation_was_interrupted(expected_generation))
        {
            result = restore_awake_runtime(&network_suspended, &voice_stopped, &ui_stopped);
            if (result != ESP_OK)
            {
                return result;
            }
            set_status(APP_POWER_STATE_AWAKE, APP_POWER_STEP_NONE, ESP_OK, ESP_OK);
            return ESP_OK;
        }

        const uint32_t blockers = collect_runtime_blockers();
        set_blockers(blockers);
        if (blockers != APP_POWER_BLOCKER_NONE)
        {
            result = restore_awake_runtime(&network_suspended, &voice_stopped, &ui_stopped);
            if (result != ESP_OK)
            {
                return result;
            }
            set_state_step(APP_POWER_STATE_AWAKE, APP_POWER_STEP_CHECK_BLOCKERS);
            return ESP_OK;
        }

        expected_generation = begin_cycle();
        result              = stop_ui_for_sleep(expected_generation);
        if (result != ESP_OK)
        {
            goto restore_awake;
        }
        ui_stopped = true;
    }

restore_awake: {
    const esp_err_t recovery_error = restore_awake_runtime(&network_suspended, &voice_stopped, &ui_stopped);
    if (recovery_error != ESP_OK)
    {
        return result == ESP_OK ? recovery_error : result;
    }
}
    if (result == ESP_ERR_INVALID_STATE)
    {
        set_status(APP_POWER_STATE_AWAKE, APP_POWER_STEP_NONE, ESP_OK, ESP_OK);
    }
    return result;
}

/** @brief 进入 BLOCKED 后等待外部停止请求 */
static void wait_until_stopped(void)
{
    while (!stop_is_requested())
    {
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

/** @brief 唯一电源 Task，拥有活动窗口、维护刷新和按键唤醒状态 */
static void app_power_task(void *arg)
{
    (void) arg;

    for (;;)
    {
        set_status(APP_POWER_STATE_AWAKE, APP_POWER_STEP_NONE, ESP_OK, ESP_OK);
        wait_awake_window();
        if (stop_is_requested())
        {
            break;
        }

        set_state_step(APP_POWER_STATE_AWAKE, APP_POWER_STEP_CHECK_BLOCKERS);
        (void) app_voice_reconcile_network_lease(APP_POWER_VOICE_LIFECYCLE_TIMEOUT_MS);
        const uint32_t blockers = collect_runtime_blockers();
        set_blockers(blockers);
        if (blockers != APP_POWER_BLOCKER_NONE)
        {
            wait_notification(s_config.retry_delay_ms);
            continue;
        }

        const bool      keep_live_display   = app_pomodoro_requires_live_display();
        const uint32_t  expected_generation = keep_live_display ? begin_offline_display_attempt() : begin_cycle();
        const esp_err_t error               = keep_live_display ? run_offline_display_session(expected_generation)
                                                                : run_sleep_session(expected_generation);
        if (error == ESP_OK)
        {
            continue;
        }
        if (stop_is_requested())
        {
            continue;
        }
        if (error == ESP_ERR_INVALID_STATE)
        {
            set_status(APP_POWER_STATE_AWAKE, APP_POWER_STEP_NONE, ESP_OK, ESP_OK);
            wait_notification(s_config.retry_delay_ms);
            continue;
        }

        taskENTER_CRITICAL(&s_lock);
        const esp_err_t primary_error  = s_primary_error;
        const esp_err_t recovery_error = s_recovery_error;
        taskEXIT_CRITICAL(&s_lock);
        enter_blocked(primary_error != ESP_OK ? primary_error : error, recovery_error);
        wait_until_stopped();
        break;
    }

    taskENTER_CRITICAL(&s_lock);
    s_state          = APP_POWER_STATE_STOPPED;
    s_step           = APP_POWER_STEP_NONE;
    s_task           = NULL;
    s_started        = false;
    s_stop_requested = false;
    taskEXIT_CRITICAL(&s_lock);
    (void) xSemaphoreGive(s_stopped_signal);
    vTaskDelete(NULL);
}

esp_err_t app_power_init(const app_power_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->automatic_light_sleep_enabled
        && (config->idle_timeout_ms == 0U || config->refresh_interval_ms == 0U || config->retry_delay_ms == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_stopped_signal = xSemaphoreCreateBinary();
    if (s_stopped_signal == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_config                  = *config;
    s_state                   = APP_POWER_STATE_STOPPED;
    s_step                    = APP_POWER_STEP_NONE;
    s_wakeup_source           = APP_POWER_WAKEUP_NONE;
    s_last_activity_us        = 0;
    s_activity_generation     = 0U;
    s_cycle_id                = 0U;
    s_success_count           = 0U;
    s_timer_refresh_count     = 0U;
    s_blockers                = APP_POWER_BLOCKER_NONE;
    s_primary_error           = ESP_OK;
    s_recovery_error          = ESP_OK;
    s_initialized             = true;
    s_started                 = false;
    s_stop_requested          = false;
    return ESP_OK;
}

esp_err_t app_power_start(void)
{
    if (!s_initialized || s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    (void) xSemaphoreTake(s_stopped_signal, 0);
    taskENTER_CRITICAL(&s_lock);
    s_started          = true;
    s_stop_requested   = false;
    s_last_activity_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);

    if (!s_config.automatic_light_sleep_enabled)
    {
        ESP_LOGI(TAG, "自动低功耗流程已关闭");
        return ESP_OK;
    }

    if (xTaskCreate(app_power_task, "app_power", APP_POWER_TASK_STACK_SIZE, NULL, APP_POWER_TASK_PRIORITY, &s_task)
        != pdPASS)
    {
        taskENTER_CRITICAL(&s_lock);
        s_started = false;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "自动低功耗流程已启动，无活动窗口=%lu ms，Timer 刷新间隔=%lu ms",
             (unsigned long) s_config.idle_timeout_ms,
             (unsigned long) s_config.refresh_interval_ms);
    return ESP_OK;
}

esp_err_t app_power_notify_activity(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (!s_initialized || !s_started)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_activity_generation++;
    s_last_activity_us      = esp_timer_get_time();
    const TaskHandle_t task = s_task;
    if (task != NULL)
    {
        xTaskNotifyGive(task);
    }
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t app_power_get_status_copy(app_power_status_t *out_status)
{
    if (out_status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_lock);
    if (!s_initialized)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_status = (app_power_status_t) {
        .state                         = s_state,
        .step                          = s_step,
        .wakeup_source                 = s_wakeup_source,
        .automatic_light_sleep_enabled = s_config.automatic_light_sleep_enabled,
        .activity_generation           = s_activity_generation,
        .cycle_id                      = s_cycle_id,
        .success_count                 = s_success_count,
        .timer_refresh_count           = s_timer_refresh_count,
        .blockers                      = s_blockers,
        .primary_error                 = s_primary_error,
        .recovery_error                = s_recovery_error,
    };
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t app_power_stop(uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_lock);
    const TaskHandle_t task = s_task;
    if (task == NULL)
    {
        s_started = false;
        s_state   = APP_POWER_STATE_STOPPED;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_stop_requested = true;
    xTaskNotifyGive(task);
    taskEXIT_CRITICAL(&s_lock);
    return xSemaphoreTake(s_stopped_signal, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_power_deinit(void)
{
    if (!s_initialized || s_started || s_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    vSemaphoreDelete(s_stopped_signal);
    s_stopped_signal          = NULL;
    s_config                  = (app_power_config_t) { 0 };
    s_state                   = APP_POWER_STATE_STOPPED;
    s_step                    = APP_POWER_STEP_NONE;
    s_wakeup_source           = APP_POWER_WAKEUP_NONE;
    s_last_activity_us        = 0;
    s_activity_generation     = 0U;
    s_cycle_id                = 0U;
    s_success_count           = 0U;
    s_timer_refresh_count     = 0U;
    s_blockers                = APP_POWER_BLOCKER_NONE;
    s_primary_error           = ESP_OK;
    s_recovery_error          = ESP_OK;
    s_initialized             = false;
    return ESP_OK;
}
