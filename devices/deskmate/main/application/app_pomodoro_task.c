/**
 * @file app_pomodoro_task.c
 * @brief 串行执行番茄钟状态机、Timer 调度、日期归一化与 Presenter 推送
 */
#include "app_pomodoro_internal.h"
#include "app_page.h"
#include "app_power.h"

#include <limits.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "pomodoro_presenter.h"
#include "presentation_dispatch.h"
#include "system_clock.h"
#include "task_stack_stats.h"

#define US_PER_SECOND 1000000LL
#define US_PER_MINUTE (60LL * US_PER_SECOND)

static const char           *TAG = "app_pomodoro_task";
static uint64_t              s_presentation_revision;
static bool                  s_dispatched_status_valid;
static app_pomodoro_status_t s_last_dispatched_status;

static uint64_t next_generation(uint64_t current)
{
    return current == UINT64_MAX ? UINT64_MAX : current + 1U;
}

static uint32_t duration_seconds_for(const app_pomodoro_state_t *state, app_pomodoro_phase_t phase)
{
    switch (phase)
    {
        case APP_POMODORO_PHASE_SHORT_BREAK:
            return (uint32_t) state->status.settings.short_break_minutes * 60U;
        case APP_POMODORO_PHASE_LONG_BREAK:
            return (uint32_t) state->status.settings.long_break_minutes * 60U;
        case APP_POMODORO_PHASE_FOCUS:
        case APP_POMODORO_PHASE_NONE:
        default:
            return (uint32_t) state->status.settings.focus_minutes * 60U;
    }
}

static pomodoro_view_phase_t presenter_phase(app_pomodoro_phase_t phase)
{
    return (pomodoro_view_phase_t) phase;
}

static pomodoro_view_run_state_t presenter_run_state(app_pomodoro_run_state_t state)
{
    return (pomodoro_view_run_state_t) state;
}

/** @brief 判断非番茄钟页面是否需要刷新状态栏、完成提示或设置视图 */
static bool status_requires_dispatch(const app_pomodoro_status_t *status)
{
    if (!s_dispatched_status_valid)
    {
        return true;
    }
    const uint32_t minutes      = (status->remaining_seconds + 59U) / 60U;
    const uint32_t last_minutes = (s_last_dispatched_status.remaining_seconds + 59U) / 60U;
    return status->phase != s_last_dispatched_status.phase || status->next_phase != s_last_dispatched_status.next_phase
           || status->run_state != s_last_dispatched_status.run_state || minutes != last_minutes
           || status->completed_in_cycle != s_last_dispatched_status.completed_in_cycle
           || status->today_focus_count != s_last_dispatched_status.today_focus_count
           || status->pending_focus_count != s_last_dispatched_status.pending_focus_count
           || status->date_verified != s_last_dispatched_status.date_verified
           || status->settings_saved != s_last_dispatched_status.settings_saved
           || status->completion_latched != s_last_dispatched_status.completion_latched
           || status->completion_generation != s_last_dispatched_status.completion_generation
           || status->last_error != s_last_dispatched_status.last_error
           || memcmp(&status->settings, &s_last_dispatched_status.settings, sizeof(status->settings)) != 0;
}

/** @brief 在状态锁外把同一版完整事实同步给 Presenter，再发布专用刷新事件 */
static void publish_current_state(void)
{
    app_pomodoro_status_t status;
    xSemaphoreTake(g_app_pomodoro.state_lock, portMAX_DELAY);
    status = g_app_pomodoro.state.status;
    xSemaphoreGive(g_app_pomodoro.state_lock);

    if (s_presentation_revision == UINT64_MAX)
    {
        ESP_LOGE(TAG, "番茄钟展示版本耗尽，停止推送");
        return;
    }
    const pomodoro_presenter_input_t input = {
        .revision              = ++s_presentation_revision,
        .phase                 = presenter_phase(status.phase),
        .next_phase            = presenter_phase(status.next_phase),
        .run_state             = presenter_run_state(status.run_state),
        .remaining_seconds     = status.remaining_seconds,
        .duration_seconds      = status.phase_duration_seconds,
        .completed_in_cycle    = status.completed_in_cycle,
        .today_count           = status.today_focus_count,
        .pending_count         = status.pending_focus_count,
        .focus_minutes         = status.settings.focus_minutes,
        .short_break_minutes   = status.settings.short_break_minutes,
        .long_break_minutes    = status.settings.long_break_minutes,
        .long_break_interval   = status.settings.long_break_interval,
        .date_verified         = status.date_verified,
        .settings_saved        = status.settings_saved,
        .completion_latched    = status.completion_latched,
        .completion_generation = status.completion_generation,
        .expected_end_valid    = status.expected_end_valid,
        .expected_end_utc      = status.expected_end_utc,
        .last_error            = status.last_error,
    };
    bool            accepted = false;
    const esp_err_t error    = pomodoro_presenter_apply(&input, &accepted);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "应用番茄钟展示快照失败: %s", esp_err_to_name(error));
        return;
    }
    if (accepted && (app_page_get_current() == PRESENTATION_PAGE_POMODORO || status_requires_dispatch(&status)))
    {
        const esp_err_t dispatch_error = presentation_dispatch_pomodoro_update();
        if (dispatch_error != ESP_OK)
        {
            ESP_LOGW(TAG, "投递番茄钟刷新失败: %s", esp_err_to_name(dispatch_error));
        }
        else
        {
            s_last_dispatched_status  = status;
            s_dispatched_status_valid = true;
        }
    }
}

static void stop_phase_timer_locked(void)
{
    const esp_err_t error = esp_timer_stop(g_app_pomodoro.phase_timer);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        g_app_pomodoro.state.status.last_error = error;
        ESP_LOGW(TAG, "停止番茄钟阶段 Timer 失败: %s", esp_err_to_name(error));
    }
}

/** @brief 把 one-shot Timer 调度到下一秒边界或阶段截止，两者取更近值 */
static esp_err_t schedule_phase_timer_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    if (state->status.run_state != APP_POMODORO_RUN_STATE_RUNNING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    stop_phase_timer_locked();
    const int64_t now_us       = esp_timer_get_time();
    const int64_t remaining_us = state->phase_deadline_us - now_us;
    if (remaining_us <= 0)
    {
        return ESP_OK;
    }
    int64_t next_boundary_us = US_PER_SECOND - (now_us % US_PER_SECOND);
    if (next_boundary_us <= 0)
    {
        next_boundary_us = US_PER_SECOND;
    }
    const uint64_t delay_us = (uint64_t) (remaining_us < next_boundary_us ? remaining_us : next_boundary_us);
    taskENTER_CRITICAL(&g_app_pomodoro.timer_lock);
    g_app_pomodoro.scheduled_generation = state->status.generation;
    taskEXIT_CRITICAL(&g_app_pomodoro.timer_lock);
    return esp_timer_start_once(g_app_pomodoro.phase_timer, delay_us == 0U ? 1U : delay_us);
}

static void update_expected_end_locked(void)
{
    app_pomodoro_status_t *status = &g_app_pomodoro.state.status;
    status->expected_end_valid    = false;
    status->expected_end_utc      = 0;
    if (status->run_state != APP_POMODORO_RUN_STATE_RUNNING)
    {
        return;
    }
    system_clock_snapshot_t clock = { 0 };
    if (system_clock_get_snapshot_copy(&clock) == ESP_OK && clock.valid)
    {
        const int64_t remaining_us = g_app_pomodoro.state.phase_deadline_us - esp_timer_get_time();
        status->expected_end_utc =
            clock.utc_timestamp + (time_t) (remaining_us > 0 ? (remaining_us + US_PER_SECOND - 1) / US_PER_SECOND : 0);
        status->expected_end_valid = true;
    }
}

static void refresh_remaining_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    if (state->status.run_state == APP_POMODORO_RUN_STATE_RUNNING)
    {
        const int64_t remaining_us = state->phase_deadline_us - esp_timer_get_time();
        state->status.remaining_seconds =
            remaining_us > 0 ? (uint32_t) ((remaining_us + US_PER_SECOND - 1) / US_PER_SECOND) : 0U;
    }
    else if (state->status.run_state == APP_POMODORO_RUN_STATE_PAUSED)
    {
        state->status.remaining_seconds =
            state->paused_remaining_us > 0
                ? (uint32_t) ((state->paused_remaining_us + US_PER_SECOND - 1) / US_PER_SECOND)
                : 0U;
    }
}

/**
 * @brief 释放状态锁提交当前计数，并在返回前重新取得状态锁
 *
 * 调用方进入和返回时都持有 state_lock。释放期间只有当前 Task 能写状态，Getter 只能读到
 * 已经完整更新、但保守标记为未保存的快照。
 */
static void save_counts_locked(void)
{
    app_pomodoro_state_t *state         = &g_app_pomodoro.state;
    const uint32_t        today_date    = state->today_date;
    const uint8_t         today_count   = state->status.today_focus_count;
    const uint8_t         pending_count = state->status.pending_focus_count;
    state->status.settings_saved        = false;
    xSemaphoreGive(g_app_pomodoro.state_lock);
    const esp_err_t error = pomodoro_store_save_counts(today_date, today_count, pending_count);
    xSemaphoreTake(g_app_pomodoro.state_lock, portMAX_DELAY);
    state->status.settings_saved = error == ESP_OK;
    state->status.last_error     = error;
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "保存番茄钟完成计数失败: %s", esp_err_to_name(error));
    }
}

/**
 * @brief 计算可信本地日期并安排下一次午夜归一化
 *
 * 调用方持有 state_lock。本函数的 NVS 提交可能失败，但内存事实仍继续使用。
 */
static void normalize_date_locked(void)
{
    app_pomodoro_state_t   *state            = &g_app_pomodoro.state;
    system_clock_snapshot_t clock            = { 0 };
    const esp_err_t         timer_stop_error = esp_timer_stop(g_app_pomodoro.date_timer);
    if (timer_stop_error != ESP_OK && timer_stop_error != ESP_ERR_INVALID_STATE)
    {
        state->status.last_error = timer_stop_error;
    }
    if (system_clock_get_snapshot_copy(&clock) != ESP_OK || !clock.valid)
    {
        state->status.date_verified = false;
        return;
    }

    struct tm local_now;
    if (localtime_r(&clock.utc_timestamp, &local_now) == NULL)
    {
        state->status.date_verified = false;
        return;
    }
    const uint32_t local_date     = (uint32_t) (local_now.tm_year + 1900) * 10000U
                                    + (uint32_t) (local_now.tm_mon + 1) * 100U + (uint32_t) local_now.tm_mday;
    bool           counts_changed = false;
    if (state->today_date != local_date)
    {
        state->today_date               = local_date;
        state->status.today_focus_count = 0U;
        counts_changed                  = true;
    }
    if (state->status.pending_focus_count > 0U)
    {
        const unsigned merged =
            (unsigned) state->status.today_focus_count + (unsigned) state->status.pending_focus_count;
        state->status.today_focus_count   = (uint8_t) (merged > UINT8_MAX ? UINT8_MAX : merged);
        state->status.pending_focus_count = 0U;
        counts_changed                    = true;
    }
    state->status.date_verified = true;
    if (counts_changed)
    {
        save_counts_locked();
    }

    struct tm next_midnight = local_now;
    next_midnight.tm_mday += 1;
    next_midnight.tm_hour  = 0;
    next_midnight.tm_min   = 0;
    next_midnight.tm_sec   = 0;
    next_midnight.tm_isdst = -1;
    const time_t next_utc  = mktime(&next_midnight);
    if (next_utc > clock.utc_timestamp)
    {
        const uint64_t  delay_us = (uint64_t) (next_utc - clock.utc_timestamp) * (uint64_t) US_PER_SECOND;
        const esp_err_t error    = esp_timer_start_once(g_app_pomodoro.date_timer, delay_us);
        if (error != ESP_OK)
        {
            state->status.last_error = error;
            ESP_LOGW(TAG, "安排番茄钟午夜 Timer 失败: %s", esp_err_to_name(error));
        }
    }
}

static esp_err_t start_phase_locked(app_pomodoro_phase_t phase)
{
    app_pomodoro_state_t *state            = &g_app_pomodoro.state;
    const uint32_t        duration_seconds = duration_seconds_for(state, phase);
    state->status.phase                    = phase;
    state->status.next_phase               = APP_POMODORO_PHASE_NONE;
    state->status.run_state                = APP_POMODORO_RUN_STATE_RUNNING;
    state->status.phase_duration_seconds   = duration_seconds;
    state->status.remaining_seconds        = duration_seconds;
    state->paused_remaining_us             = (int64_t) duration_seconds * US_PER_SECOND;
    state->phase_deadline_us               = esp_timer_get_time() + state->paused_remaining_us;
    state->status.completion_latched       = false;
    state->status.completion_generation    = 0U;
    state->status.generation               = next_generation(state->status.generation);
    state->status.last_error               = ESP_OK;
    update_expected_end_locked();
    const esp_err_t error = schedule_phase_timer_locked();
    if (error != ESP_OK)
    {
        state->status.run_state          = APP_POMODORO_RUN_STATE_PAUSED;
        state->status.expected_end_valid = false;
        state->status.last_error         = error;
        ESP_LOGE(TAG, "启动番茄钟阶段 Timer 失败: %s", esp_err_to_name(error));
    }
    return error;
}

static void reset_to_idle_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    stop_phase_timer_locked();
    state->status.phase                  = APP_POMODORO_PHASE_NONE;
    state->status.next_phase             = APP_POMODORO_PHASE_FOCUS;
    state->status.run_state              = APP_POMODORO_RUN_STATE_IDLE;
    state->status.completed_in_cycle     = 0U;
    state->status.phase_duration_seconds = duration_seconds_for(state, APP_POMODORO_PHASE_FOCUS);
    state->status.remaining_seconds      = state->status.phase_duration_seconds;
    state->status.completion_latched     = false;
    state->status.completion_generation  = 0U;
    state->status.expected_end_valid     = false;
    state->status.expected_end_utc       = 0;
    state->status.last_error             = ESP_OK;
    state->paused_remaining_us           = 0;
    state->phase_deadline_us             = 0;
    state->status.generation             = next_generation(state->status.generation);
}

/**
 * @brief 把一次自然完成的专注归入可信今日或未定日计数并提交 NVS
 *
 * 调用方必须持有 state_lock；写入失败只更新错误事实，不撤销内存中的完成数。
 */
static void persist_completed_focus_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    normalize_date_locked();
    if (state->status.date_verified)
    {
        if (state->status.today_focus_count < UINT8_MAX)
        {
            state->status.today_focus_count++;
        }
    }
    else if (state->status.pending_focus_count < UINT8_MAX)
    {
        state->status.pending_focus_count++;
    }
    save_counts_locked();
}

/**
 * @brief 幂等完成当前运行阶段并锁存下一阶段
 *
 * 专注阶段在同一串行临界区更新周期轮次和持久化完成计数；休息阶段只决定下一次专注。
 * 调用方必须持有 state_lock，且仅允许业务 Task 调用。
 */
static void complete_phase_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    if (state->status.run_state != APP_POMODORO_RUN_STATE_RUNNING)
    {
        return;
    }
    stop_phase_timer_locked();
    state->status.remaining_seconds     = 0U;
    state->status.run_state             = APP_POMODORO_RUN_STATE_DONE;
    state->status.expected_end_valid    = false;
    state->status.generation            = next_generation(state->status.generation);
    state->status.completion_latched    = true;
    state->status.completion_generation = state->status.generation;
    if (state->status.phase == APP_POMODORO_PHASE_FOCUS)
    {
        if (state->status.completed_in_cycle < UINT8_MAX)
        {
            state->status.completed_in_cycle++;
        }
        persist_completed_focus_locked();
        state->status.next_phase = state->status.completed_in_cycle >= state->status.settings.long_break_interval
                                       ? APP_POMODORO_PHASE_LONG_BREAK
                                       : APP_POMODORO_PHASE_SHORT_BREAK;
    }
    else
    {
        state->status.next_phase = APP_POMODORO_PHASE_FOCUS;
    }
}

/**
 * @brief 在 RUNNING 与 PAUSED 间切换并重新建立单调 deadline
 *
 * 调用方必须持有 state_lock；暂停保存相对剩余微秒，恢复不使用系统 UTC。
 */
static void pause_or_resume_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    if (state->status.run_state == APP_POMODORO_RUN_STATE_RUNNING)
    {
        state->paused_remaining_us = state->phase_deadline_us - esp_timer_get_time();
        if (state->paused_remaining_us <= 0)
        {
            complete_phase_locked();
            return;
        }
        stop_phase_timer_locked();
        state->status.run_state          = APP_POMODORO_RUN_STATE_PAUSED;
        state->status.expected_end_valid = false;
        state->status.generation         = next_generation(state->status.generation);
        state->status.last_error         = ESP_OK;
        refresh_remaining_locked();
        return;
    }
    if (state->status.run_state == APP_POMODORO_RUN_STATE_PAUSED)
    {
        state->phase_deadline_us = esp_timer_get_time() + state->paused_remaining_us;
        state->status.run_state  = APP_POMODORO_RUN_STATE_RUNNING;
        state->status.generation = next_generation(state->status.generation);
        state->status.last_error = ESP_OK;
        update_expected_end_locked();
        const esp_err_t error = schedule_phase_timer_locked();
        if (error != ESP_OK)
        {
            state->status.run_state          = APP_POMODORO_RUN_STATE_PAUSED;
            state->status.expected_end_valid = false;
            state->status.last_error         = error;
        }
    }
}

static void skip_phase_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    if (state->status.run_state != APP_POMODORO_RUN_STATE_RUNNING)
    {
        return;
    }
    app_pomodoro_phase_t next = APP_POMODORO_PHASE_FOCUS;
    if (state->status.phase == APP_POMODORO_PHASE_FOCUS)
    {
        next = APP_POMODORO_PHASE_SHORT_BREAK;
    }
    else if (state->status.phase == APP_POMODORO_PHASE_LONG_BREAK)
    {
        state->status.completed_in_cycle = 0U;
    }
    (void) start_phase_locked(next);
}

static void confirm_done_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    if (state->status.run_state != APP_POMODORO_RUN_STATE_DONE)
    {
        return;
    }
    if (state->status.phase == APP_POMODORO_PHASE_LONG_BREAK)
    {
        state->status.completed_in_cycle = 0U;
    }
    (void) start_phase_locked(state->status.next_phase);
}

static void update_settings_locked(const app_pomodoro_settings_t *settings)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    if (state->status.run_state != APP_POMODORO_RUN_STATE_IDLE)
    {
        state->status.last_error = ESP_ERR_INVALID_STATE;
        return;
    }
    state->status.settings                 = *settings;
    state->status.phase_duration_seconds   = duration_seconds_for(state, APP_POMODORO_PHASE_FOCUS);
    state->status.remaining_seconds        = state->status.phase_duration_seconds;
    const pomodoro_store_settings_t stored = {
        .focus_minutes       = settings->focus_minutes,
        .short_break_minutes = settings->short_break_minutes,
        .long_break_minutes  = settings->long_break_minutes,
        .long_break_interval = settings->long_break_interval,
    };
    state->status.settings_saved = false;
    xSemaphoreGive(g_app_pomodoro.state_lock);
    const esp_err_t error = pomodoro_store_save_settings_copy(&stored);
    xSemaphoreTake(g_app_pomodoro.state_lock, portMAX_DELAY);
    state->status.settings_saved = error == ESP_OK;
    state->status.last_error     = error;
    state->status.generation     = next_generation(state->status.generation);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "保存番茄钟设置失败，继续使用内存值: %s", esp_err_to_name(error));
    }
}

/**
 * @brief 按当前单调时间补算一次睡眠跨度并重调阶段 Timer
 *
 * 调用方必须持有 state_lock；本函数不发布事件，结果由命令处理器在锁外统一呈现。
 */
static app_pomodoro_wakeup_result_t reconcile_locked(void)
{
    app_pomodoro_state_t *state = &g_app_pomodoro.state;
    if (state->status.run_state != APP_POMODORO_RUN_STATE_RUNNING)
    {
        return APP_POMODORO_WAKEUP_NO_CHANGE;
    }
    refresh_remaining_locked();
    if (state->phase_deadline_us <= esp_timer_get_time())
    {
        complete_phase_locked();
        return APP_POMODORO_WAKEUP_PHASE_COMPLETED;
    }
    const esp_err_t error = schedule_phase_timer_locked();
    if (error != ESP_OK)
    {
        state->status.run_state          = APP_POMODORO_RUN_STATE_PAUSED;
        state->status.expected_end_valid = false;
        state->status.last_error         = error;
    }
    return APP_POMODORO_WAKEUP_RESCHEDULED;
}

/**
 * @brief 在唯一业务 Task 中解释一条命令并控制锁外 Presenter 推送
 *
 * 同步补算命令必须先发布已经收敛的 Presenter 快照，再给出 reconcile_sem 回执，保证 Power
 * Application 恢复 UI 时不会读到旧状态。
 *
 * @param[in] command 队列按值复制的命令
 * @return true 继续处理命令；false 已执行 STOP，应退出 Task
 */
static bool handle_command(const app_pomodoro_command_t *command)
{
    bool publish                    = true;
    bool notify_completion_activity = false;
    xSemaphoreTake(g_app_pomodoro.state_lock, portMAX_DELAY);
    switch (command->type)
    {
        case APP_POMODORO_COMMAND_START:
            if (g_app_pomodoro.state.status.run_state == APP_POMODORO_RUN_STATE_IDLE)
            {
                (void) start_phase_locked(APP_POMODORO_PHASE_FOCUS);
            }
            else
            {
                g_app_pomodoro.state.status.last_error = ESP_ERR_INVALID_STATE;
            }
            break;
        case APP_POMODORO_COMMAND_TOGGLE_PAUSE:
            if (g_app_pomodoro.state.status.run_state == APP_POMODORO_RUN_STATE_RUNNING
                || g_app_pomodoro.state.status.run_state == APP_POMODORO_RUN_STATE_PAUSED)
            {
                pause_or_resume_locked();
            }
            else
            {
                g_app_pomodoro.state.status.last_error = ESP_ERR_INVALID_STATE;
            }
            break;
        case APP_POMODORO_COMMAND_SKIP:
            if (g_app_pomodoro.state.status.run_state == APP_POMODORO_RUN_STATE_RUNNING)
            {
                skip_phase_locked();
            }
            else
            {
                g_app_pomodoro.state.status.last_error = ESP_ERR_INVALID_STATE;
            }
            break;
        case APP_POMODORO_COMMAND_CONFIRM:
            if (g_app_pomodoro.state.status.run_state == APP_POMODORO_RUN_STATE_DONE)
            {
                confirm_done_locked();
            }
            else
            {
                g_app_pomodoro.state.status.last_error = ESP_ERR_INVALID_STATE;
            }
            break;
        case APP_POMODORO_COMMAND_RESET:
            if (g_app_pomodoro.state.status.run_state == APP_POMODORO_RUN_STATE_PAUSED
                || g_app_pomodoro.state.status.run_state == APP_POMODORO_RUN_STATE_DONE)
            {
                reset_to_idle_locked();
            }
            else
            {
                g_app_pomodoro.state.status.last_error = ESP_ERR_INVALID_STATE;
            }
            break;
        case APP_POMODORO_COMMAND_UPDATE_SETTINGS:
            update_settings_locked(&command->settings);
            break;
        case APP_POMODORO_COMMAND_TICK:
            if (command->generation != g_app_pomodoro.state.status.generation
                || g_app_pomodoro.state.status.run_state != APP_POMODORO_RUN_STATE_RUNNING)
            {
                publish = false;
                break;
            }
            refresh_remaining_locked();
            if (g_app_pomodoro.state.phase_deadline_us <= esp_timer_get_time())
            {
                complete_phase_locked();
                notify_completion_activity = true;
            }
            else
            {
                const esp_err_t error = schedule_phase_timer_locked();
                if (error != ESP_OK)
                {
                    g_app_pomodoro.state.status.run_state          = APP_POMODORO_RUN_STATE_PAUSED;
                    g_app_pomodoro.state.status.expected_end_valid = false;
                    g_app_pomodoro.state.status.last_error         = error;
                }
            }
            break;
        case APP_POMODORO_COMMAND_NORMALIZE_DATE:
            normalize_date_locked();
            update_expected_end_locked();
            break;
        case APP_POMODORO_COMMAND_RECONCILE:
            g_app_pomodoro.reconcile_result     = reconcile_locked();
            g_app_pomodoro.completed_request_id = command->request_id;
            publish                             = g_app_pomodoro.reconcile_result != APP_POMODORO_WAKEUP_NO_CHANGE;
            break;
        case APP_POMODORO_COMMAND_STOP:
            stop_phase_timer_locked();
            (void) esp_timer_stop(g_app_pomodoro.date_timer);
            xSemaphoreGive(g_app_pomodoro.state_lock);
            return false;
        default:
            publish = false;
            break;
    }
    const bool reconcile = command->type == APP_POMODORO_COMMAND_RECONCILE;
    xSemaphoreGive(g_app_pomodoro.state_lock);
    if (publish)
    {
        publish_current_state();
    }
    if (notify_completion_activity)
    {
        const esp_err_t activity_error = app_power_notify_activity();
        if (activity_error != ESP_OK && activity_error != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "番茄钟自然完成后更新活动窗口失败: %s", esp_err_to_name(activity_error));
        }
    }
    if (reconcile)
    {
        (void) xSemaphoreGive(g_app_pomodoro.reconcile_sem);
    }
    return true;
}

void app_pomodoro_task(void *arg)
{
    (void) arg;
    task_stack_stats_t stack_stats = TASK_STACK_STATS_INITIALIZER;
    xSemaphoreTake(g_app_pomodoro.state_lock, portMAX_DELAY);
    normalize_date_locked();
    (void) reconcile_locked();
    xSemaphoreGive(g_app_pomodoro.state_lock);
    publish_current_state();
    (void) xSemaphoreGive(g_app_pomodoro.ready_sem);

    app_pomodoro_command_t command;
    while (xQueueReceive(g_app_pomodoro.queue, &command, portMAX_DELAY) == pdTRUE)
    {
        task_stack_stats_log_if_due(&stack_stats, "app_pomodoro_task");
        if (!handle_command(&command))
        {
            break;
        }
    }

    task_stack_stats_log_now("app_pomodoro_task");
    g_app_pomodoro.task = NULL;
    (void) xSemaphoreGive(g_app_pomodoro.stopped_sem);
    vTaskDelete(NULL);
}
