#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*audio_processor_feed_step_t)(void);
typedef bool (*audio_processor_fetch_step_t)(bool draining);

typedef struct
{
    bool tasks_created;
    bool feed_parked;
    bool fetch_parked;
} audio_processor_task_status_t;

esp_err_t audio_processor_task_runtime_init(audio_processor_feed_step_t feed_step,
                                            audio_processor_fetch_step_t fetch_step);
esp_err_t audio_processor_task_runtime_ensure_created(void);
void      audio_processor_task_runtime_begin_processing(void);
void      audio_processor_task_runtime_begin_drain(void);
esp_err_t audio_processor_task_runtime_wait_drain_and_park(uint32_t timeout_ms);
esp_err_t audio_processor_task_runtime_park(uint32_t timeout_ms);
esp_err_t audio_processor_task_runtime_deinit(uint32_t timeout_ms);
void      audio_processor_task_runtime_get_status(audio_processor_task_status_t *out_status);
