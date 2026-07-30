/**
 * @file app_pomodoro_internal.h
 * @brief 番茄钟公共入口与唯一业务 Task 之间的私有协作契约
 */
#pragma once

#include "app_pomodoro.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pomodoro_store.h"

typedef enum
{
    APP_POMODORO_COMMAND_START = 0,
    APP_POMODORO_COMMAND_TOGGLE_PAUSE,
    APP_POMODORO_COMMAND_SKIP,
    APP_POMODORO_COMMAND_CONFIRM,
    APP_POMODORO_COMMAND_RESET,
    APP_POMODORO_COMMAND_UPDATE_SETTINGS,
    APP_POMODORO_COMMAND_TICK,
    APP_POMODORO_COMMAND_NORMALIZE_DATE,
    APP_POMODORO_COMMAND_RECONCILE,
    APP_POMODORO_COMMAND_STOP,
} app_pomodoro_command_type_t;

typedef struct
{
    app_pomodoro_command_type_t    type;
    app_pomodoro_settings_update_t settings_update;
    uint64_t                       settings_request_id;
    uint64_t                       generation;
    uint32_t                       reconcile_request_id;
} app_pomodoro_command_t;

typedef struct
{
    app_pomodoro_snapshot_t snapshot;
    int64_t                 phase_deadline_us;
    int64_t                 paused_remaining_us;
    uint32_t                today_date;
} app_pomodoro_runtime_data_t;

typedef struct
{
    QueueHandle_t                        queue;
    SemaphoreHandle_t                    state_lock;
    SemaphoreHandle_t                    ready_sem;
    SemaphoreHandle_t                    stopped_sem;
    SemaphoreHandle_t                    reconcile_lock;
    SemaphoreHandle_t                    reconcile_sem;
    TaskHandle_t                         task;
    esp_timer_handle_t                   phase_timer;
    esp_timer_handle_t                   date_timer;
    portMUX_TYPE                         timer_lock;
    uint64_t                             scheduled_generation;
    app_pomodoro_runtime_data_t          runtime_data;
    uint32_t                             next_reconcile_request_id;
    uint32_t                             completed_reconcile_request_id;
    app_pomodoro_wakeup_result_t         reconcile_result;
    uint64_t                             next_settings_request_id;
    uint64_t                             latest_settings_request_id;
    app_pomodoro_settings_update_result_t latest_settings_update_result;
    bool                                 latest_settings_update_result_valid;
    bool                                 initialized;
    bool                                 running;
    bool                                 stopping;
} app_pomodoro_runtime_t;

extern app_pomodoro_runtime_t g_app_pomodoro_runtime;

/** @brief 番茄钟唯一业务 Task 入口 */
void app_pomodoro_task(void *arg);

/** @brief 校验完整番茄钟设置范围和步长 */
bool app_pomodoro_settings_are_valid(const app_pomodoro_settings_t *settings);
