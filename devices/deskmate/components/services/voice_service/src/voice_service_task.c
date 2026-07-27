#include "voice_service_internal.h"

#include <stdint.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define VOICE_CHAT_TASK_STACK     12288
#define VOICE_PLAYBACK_TASK_STACK 4096
#define VOICE_CHAT_TASK_PRIORITY  2
#define VOICE_PLAY_TASK_PRIORITY  (VOICE_CHAT_TASK_PRIORITY + 1)

typedef struct
{
    voice_service_task_run_t run;
    void                    *arg;
} voice_service_task_request_t;

static portMUX_TYPE                 s_task_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t                 s_chat_task;
static TaskHandle_t                 s_playback_task;
static voice_service_task_request_t s_chat_request;
static voice_service_task_request_t s_playback_request;

/** @brief 一次性语音会话 Task 入口，业务函数返回后统一清除句柄并删除自身。 */
static void voice_service_chat_task(void *arg)
{
    voice_service_task_request_t request = *(voice_service_task_request_t *) arg;
    request.run(request.arg);
    taskENTER_CRITICAL(&s_task_lock);
    s_chat_task = NULL;
    taskEXIT_CRITICAL(&s_task_lock);
    vTaskDeleteWithCaps(NULL);
}

/** @brief 一次性播放 Task 入口，业务函数返回后统一清除句柄并删除自身。 */
static void voice_service_playback_task(void *arg)
{
    voice_service_task_request_t request = *(voice_service_task_request_t *) arg;
    request.run(request.arg);
    taskENTER_CRITICAL(&s_task_lock);
    s_playback_task = NULL;
    taskEXIT_CRITICAL(&s_task_lock);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t voice_service_task_start_chat(voice_service_task_run_t run, void *arg)
{
    if (run == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_task_lock);
    if (s_chat_task != NULL)
    {
        taskEXIT_CRITICAL(&s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_task_lock);
    s_chat_request = (voice_service_task_request_t) {
        .run = run,
        .arg = arg,
    };
    const BaseType_t result = xTaskCreateWithCaps(voice_service_chat_task,
                                                  "voice_chat",
                                                  VOICE_CHAT_TASK_STACK,
                                                  &s_chat_request,
                                                  VOICE_CHAT_TASK_PRIORITY,
                                                  &s_chat_task,
                                                  MALLOC_CAP_SPIRAM);
    if (result != pdPASS)
    {
        taskENTER_CRITICAL(&s_task_lock);
        s_chat_task = NULL;
        taskEXIT_CRITICAL(&s_task_lock);
    }
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t voice_service_task_start_playback(voice_service_task_run_t run, void *arg)
{
    if (run == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_task_lock);
    if (s_playback_task != NULL)
    {
        taskEXIT_CRITICAL(&s_task_lock);
        return ESP_ERR_INVALID_STATE;
    }
    taskEXIT_CRITICAL(&s_task_lock);
    s_playback_request = (voice_service_task_request_t) {
        .run = run,
        .arg = arg,
    };
    const BaseType_t result = xTaskCreateWithCaps(voice_service_playback_task,
                                                  "voice_play",
                                                  VOICE_PLAYBACK_TASK_STACK,
                                                  &s_playback_request,
                                                  VOICE_PLAY_TASK_PRIORITY,
                                                  &s_playback_task,
                                                  MALLOC_CAP_SPIRAM);
    if (result != pdPASS)
    {
        taskENTER_CRITICAL(&s_task_lock);
        s_playback_task = NULL;
        taskEXIT_CRITICAL(&s_task_lock);
    }
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void voice_service_task_get_status(voice_service_task_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }
    taskENTER_CRITICAL(&s_task_lock);
    *out_status = (voice_service_task_status_t) {
        .chat_task_active     = s_chat_task != NULL,
        .playback_task_active = s_playback_task != NULL,
    };
    taskEXIT_CRITICAL(&s_task_lock);
}
