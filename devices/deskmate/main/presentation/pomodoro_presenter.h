/**
 * @file pomodoro_presenter.h
 * @brief 声明番茄钟纯展示事实与线程安全 View Model
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

/** @brief 番茄钟 Presenter 阶段 */
typedef enum
{
    POMODORO_VIEW_PHASE_NONE = 0,
    POMODORO_VIEW_PHASE_FOCUS,
    POMODORO_VIEW_PHASE_SHORT_BREAK,
    POMODORO_VIEW_PHASE_LONG_BREAK,
} pomodoro_view_phase_t;

/** @brief 番茄钟 Presenter 运行状态 */
typedef enum
{
    POMODORO_VIEW_RUN_IDLE = 0,
    POMODORO_VIEW_RUN_RUNNING,
    POMODORO_VIEW_RUN_PAUSED,
    POMODORO_VIEW_RUN_DONE,
} pomodoro_view_run_state_t;

/** @brief 番茄钟设置异步更新的 Presenter 状态 */
typedef enum
{
    POMODORO_VIEW_SETTINGS_UPDATE_PENDING = 0,
    POMODORO_VIEW_SETTINGS_UPDATE_SUCCEEDED,
    POMODORO_VIEW_SETTINGS_UPDATE_FAILED,
} pomodoro_view_settings_update_state_t;

/** @brief Application 按值推送的完整番茄钟展示事实 */
typedef struct
{
    uint64_t                  revision;
    pomodoro_view_phase_t     phase;
    pomodoro_view_phase_t     next_phase;
    pomodoro_view_run_state_t run_state;
    uint32_t                  remaining_seconds;
    uint32_t                  duration_seconds;
    uint8_t                   completed_in_cycle;
    uint8_t                   today_count;
    uint8_t                   pending_count;
    uint8_t                   focus_minutes;
    uint8_t                   short_break_minutes;
    uint8_t                   long_break_minutes;
    uint8_t                   long_break_interval;
    uint64_t                  settings_version;
    bool                      settings_update_result_valid;
    uint64_t                  settings_update_request_id;
    pomodoro_view_settings_update_state_t settings_update_state;
    uint64_t                  settings_update_version;
    esp_err_t                 settings_update_error;
    bool                      date_verified;
    bool                      settings_saved;
    bool                      completion_latched;
    uint64_t                  completion_generation;
    bool                      expected_end_valid;
    time_t                    expected_end_utc;
    esp_err_t                 last_error;
} pomodoro_presenter_input_t;

/** @brief UI 按值读取的番茄钟 View Model */
typedef struct
{
    pomodoro_view_phase_t     phase;
    pomodoro_view_phase_t     next_phase;
    pomodoro_view_run_state_t run_state;
    uint32_t                  remaining_seconds;
    uint32_t                  duration_seconds;
    uint8_t                   completed_in_cycle;
    uint8_t                   today_count;
    uint8_t                   pending_count;
    uint8_t                   focus_minutes;
    uint8_t                   short_break_minutes;
    uint8_t                   long_break_minutes;
    uint8_t                   long_break_interval;
    uint64_t                  settings_version;
    bool                      settings_update_result_valid;
    uint64_t                  settings_update_request_id;
    pomodoro_view_settings_update_state_t settings_update_state;
    uint64_t                  settings_update_version;
    esp_err_t                 settings_update_error;
    bool                      date_verified;
    bool                      settings_saved;
    bool                      completion_latched;
    uint64_t                  completion_generation;
    char                      phase_text[16];
    char                      time_text[8];
    char                      count_text[32];
    char                      end_text[40];
    char                      cycle_text[48];
    char                      hint_text[48];
    char                      completion_text[64];
    esp_err_t                 last_error;
} pomodoro_view_model_t;

/** @brief 初始化番茄钟 Presenter；@return ESP_OK 初始化完成 */
esp_err_t pomodoro_presenter_init(void);

/**
 * @brief 同步应用完整展示事实
 *
 * @param[in] input 调用期间借用的完整展示事实
 * @param[out] out_accepted 是否因版本更新而替换快照
 * @return ESP_OK 已仲裁；ESP_ERR_INVALID_ARG 参数或枚举非法；
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t pomodoro_presenter_apply(const pomodoro_presenter_input_t *input, bool *out_accepted);

/**
 * @brief 按值读取番茄钟 View Model
 *
 * @param[out] out_view 调用方提供的完整输出
 * @return ESP_OK 已复制；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t pomodoro_presenter_get_view_copy(pomodoro_view_model_t *out_view);
