/**
 * @file pomodoro_presenter.c
 * @brief 把番茄钟完整展示事实转换为线程安全文本 View Model
 */
#include "pomodoro_presenter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_bar_presenter.h"

static portMUX_TYPE          s_lock = portMUX_INITIALIZER_UNLOCKED;
static pomodoro_view_model_t s_view;
static uint64_t              s_revision;
static bool                  s_initialized;

static const char *phase_text(pomodoro_view_phase_t phase, pomodoro_view_run_state_t run_state)
{
    if (run_state == POMODORO_VIEW_RUN_IDLE)
    {
        return "准备专注";
    }
    switch (phase)
    {
        case POMODORO_VIEW_PHASE_FOCUS:
            return "专注";
        case POMODORO_VIEW_PHASE_SHORT_BREAK:
            return "短休";
        case POMODORO_VIEW_PHASE_LONG_BREAK:
            return "长休";
        case POMODORO_VIEW_PHASE_NONE:
        default:
            return "准备专注";
    }
}

static const char *next_phase_text(pomodoro_view_phase_t phase)
{
    switch (phase)
    {
        case POMODORO_VIEW_PHASE_SHORT_BREAK:
            return "短休";
        case POMODORO_VIEW_PHASE_LONG_BREAK:
            return "长休";
        case POMODORO_VIEW_PHASE_FOCUS:
        default:
            return "专注";
    }
}

static status_bar_pomodoro_state_t status_bar_state_for(pomodoro_view_run_state_t state)
{
    switch (state)
    {
        case POMODORO_VIEW_RUN_RUNNING:
            return STATUS_BAR_POMODORO_RUNNING;
        case POMODORO_VIEW_RUN_PAUSED:
            return STATUS_BAR_POMODORO_PAUSED;
        case POMODORO_VIEW_RUN_DONE:
            return STATUS_BAR_POMODORO_DONE;
        case POMODORO_VIEW_RUN_IDLE:
        default:
            return STATUS_BAR_POMODORO_HIDDEN;
    }
}

/** @brief 根据运行状态生成页面底部按键提示 */
static const char *hint_for(pomodoro_view_run_state_t state)
{
    switch (state)
    {
        case POMODORO_VIEW_RUN_RUNNING:
            return "左长暂停 / 右长跳过";
        case POMODORO_VIEW_RUN_PAUSED:
            return "左长继续 / 右长重置";
        case POMODORO_VIEW_RUN_DONE:
            return "左长取消 / 右长确认";
        case POMODORO_VIEW_RUN_IDLE:
        default:
            return "长按左键开始";
    }
}

/** @brief 把任意秒数安全换算为向上取整的分钟，避免展示层整数溢出 */
static uint32_t ceil_minutes(uint32_t seconds)
{
    return (seconds / 60U) + ((seconds % 60U) != 0U ? 1U : 0U);
}

static void build_view(const pomodoro_presenter_input_t *input, pomodoro_view_model_t *view)
{
    memset(view, 0, sizeof(*view));
    view->phase                 = input->phase;
    view->next_phase            = input->next_phase;
    view->run_state             = input->run_state;
    view->remaining_seconds     = input->remaining_seconds;
    view->duration_seconds      = input->duration_seconds;
    view->completed_in_cycle    = input->completed_in_cycle;
    view->today_count           = input->today_count;
    view->pending_count         = input->pending_count;
    view->focus_minutes         = input->focus_minutes;
    view->short_break_minutes   = input->short_break_minutes;
    view->long_break_minutes    = input->long_break_minutes;
    view->long_break_interval   = input->long_break_interval;
    view->settings_version      = input->settings_version;
    view->settings_update_result_valid = input->settings_update_result_valid;
    view->settings_update_request_id   = input->settings_update_request_id;
    view->settings_update_state        = input->settings_update_state;
    view->settings_update_version      = input->settings_update_version;
    view->settings_update_error        = input->settings_update_error;
    view->date_verified         = input->date_verified;
    view->settings_saved        = input->settings_saved;
    view->completion_latched    = input->completion_latched;
    view->completion_generation = input->completion_generation;
    view->last_error            = input->last_error;

    (void) snprintf(view->phase_text, sizeof(view->phase_text), "%s", phase_text(input->phase, input->run_state));
    const uint32_t remaining_minutes = input->remaining_seconds / 60U;
    const unsigned display_minutes   = remaining_minutes <= 999U ? (unsigned) remaining_minutes : 999U;
    (void) snprintf(view->time_text,
                    sizeof(view->time_text),
                    "%02u:%02u",
                    display_minutes,
                    (unsigned) (input->remaining_seconds % 60U));
    const unsigned total_count = (unsigned) input->today_count + (unsigned) input->pending_count;
    (void) snprintf(view->count_text,
                    sizeof(view->count_text),
                    input->date_verified ? "今日 %u 轮" : "已完成 %u 轮",
                    total_count);

    if (input->run_state == POMODORO_VIEW_RUN_IDLE)
    {
        (void) snprintf(view->end_text, sizeof(view->end_text), "准备专注");
    }
    else if (input->expected_end_valid && input->run_state == POMODORO_VIEW_RUN_RUNNING)
    {
        struct tm local_end;
        if (localtime_r(&input->expected_end_utc, &local_end) != NULL)
        {
            (void) snprintf(view->end_text,
                            sizeof(view->end_text),
                            "预计 %02d:%02d 结束",
                            local_end.tm_hour,
                            local_end.tm_min);
        }
    }
    if (view->end_text[0] == '\0')
    {
        const uint32_t minutes = ceil_minutes(input->remaining_seconds);
        (void) snprintf(view->end_text, sizeof(view->end_text), "约 %lu 分钟后结束", (unsigned long) minutes);
    }

    const uint8_t current_round = input->completed_in_cycle < input->long_break_interval
                                      ? (uint8_t) (input->completed_in_cycle + 1U)
                                      : input->long_break_interval;
    (void) snprintf(view->cycle_text,
                    sizeof(view->cycle_text),
                    "第 %u / %u 轮 · %s",
                    (unsigned) current_round,
                    (unsigned) input->long_break_interval,
                    input->run_state == POMODORO_VIEW_RUN_DONE ? "等待确认"
                                                               : phase_text(input->phase, input->run_state));
    (void) snprintf(view->hint_text, sizeof(view->hint_text), "%s", hint_for(input->run_state));
    if (input->run_state == POMODORO_VIEW_RUN_DONE)
    {
        (void) snprintf(view->completion_text,
                        sizeof(view->completion_text),
                        "已完成 %lu 分钟，下一阶段：%s",
                        (unsigned long) (input->duration_seconds / 60U),
                        next_phase_text(input->next_phase));
    }
}

esp_err_t pomodoro_presenter_init(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (!s_initialized)
    {
        memset(&s_view, 0, sizeof(s_view));
        s_revision    = 0U;
        s_initialized = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t pomodoro_presenter_apply(const pomodoro_presenter_input_t *input, bool *out_accepted)
{
    if (input == NULL || out_accepted == NULL || input->revision == 0U
        || (unsigned) input->phase > POMODORO_VIEW_PHASE_LONG_BREAK
        || (unsigned) input->next_phase > POMODORO_VIEW_PHASE_LONG_BREAK
        || (unsigned) input->run_state > POMODORO_VIEW_RUN_DONE || input->settings_version == 0U
        || (input->settings_update_result_valid
            && (input->settings_update_request_id == 0U || input->settings_update_version == 0U
                || (unsigned) input->settings_update_state > POMODORO_VIEW_SETTINGS_UPDATE_FAILED
                || ((input->settings_update_state == POMODORO_VIEW_SETTINGS_UPDATE_PENDING
                     || input->settings_update_state == POMODORO_VIEW_SETTINGS_UPDATE_SUCCEEDED)
                    && input->settings_update_error != ESP_OK)
                || (input->settings_update_state == POMODORO_VIEW_SETTINGS_UPDATE_FAILED
                    && input->settings_update_error == ESP_OK)))
        || input->long_break_interval < 2U
        || input->long_break_interval > 12U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    pomodoro_view_model_t next;
    build_view(input, &next);
    *out_accepted = false;
    taskENTER_CRITICAL(&s_lock);
    if (!s_initialized)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (input->revision > s_revision)
    {
        s_view        = next;
        s_revision    = input->revision;
        *out_accepted = true;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (*out_accepted)
    {
        const uint32_t rounded_minutes = ceil_minutes(input->remaining_seconds);
        const uint16_t minutes         = rounded_minutes <= UINT16_MAX ? (uint16_t) rounded_minutes : UINT16_MAX;
        status_bar_presenter_set_pomodoro(status_bar_state_for(input->run_state), minutes);
    }
    return ESP_OK;
}

esp_err_t pomodoro_presenter_get_view_copy(pomodoro_view_model_t *out_view)
{
    if (out_view == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    if (!s_initialized)
    {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_view = s_view;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
