#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef void (*voice_service_task_run_t)(void *arg);

typedef struct
{
    bool chat_task_active;
    bool playback_task_active;
} voice_service_task_status_t;

esp_err_t voice_service_task_start_chat(voice_service_task_run_t run, void *arg);
esp_err_t voice_service_task_start_playback(voice_service_task_run_t run, void *arg);
void      voice_service_task_get_status(voice_service_task_status_t *out_status);
