#include "audio_processor_service_internal.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "audio_processor_task";

#define APS_FEED_TASK_STACK    4096
#define APS_FEED_TASK_PRIO     3
#define APS_FETCH_TASK_STACK   4096
#define APS_FETCH_TASK_PRIO    4

#define APS_TASK_FEED_RUN      BIT0
#define APS_TASK_FETCH_RUN     BIT1
#define APS_TASK_EXIT_REQUEST  BIT2
#define APS_TASK_FEED_PARKED   BIT3
#define APS_TASK_FETCH_PARKED  BIT4
#define APS_TASK_DRAIN_REQUEST BIT5
#define APS_TASK_DRAIN_DONE    BIT6
#define APS_TASK_FEED_EXITED   BIT7
#define APS_TASK_FETCH_EXITED  BIT8

static EventGroupHandle_t           s_events;
static TaskHandle_t                 s_feed_task;
static TaskHandle_t                 s_fetch_task;
static portMUX_TYPE                 s_task_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_processor_feed_step_t  s_feed_step;
static audio_processor_fetch_step_t s_fetch_step;

static TickType_t timeout_ticks(uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
}

static EventBits_t created_parked_mask(void)
{
    taskENTER_CRITICAL(&s_task_lock);
    EventBits_t mask = 0;
    if (s_feed_task != NULL)
    {
        mask |= APS_TASK_FEED_PARKED;
    }
    if (s_fetch_task != NULL)
    {
        mask |= APS_TASK_FETCH_PARKED;
    }
    taskEXIT_CRITICAL(&s_task_lock);
    return mask;
}

/**
 * @brief AFE feed Task；仅在 FEED_RUN 有效时读取一块麦克风数据，空闲时无限阻塞
 */
static void audio_processor_feed_task(void *arg)
{
    (void) arg;
    xEventGroupSetBits(s_events, APS_TASK_FEED_PARKED);
    for (;;)
    {
        const EventBits_t bits =
            xEventGroupWaitBits(s_events, APS_TASK_FEED_RUN | APS_TASK_EXIT_REQUEST, pdFALSE, pdFALSE, portMAX_DELAY);
        if ((bits & APS_TASK_EXIT_REQUEST) != 0)
        {
            break;
        }

        xEventGroupClearBits(s_events, APS_TASK_FEED_PARKED);
        while ((xEventGroupGetBits(s_events) & (APS_TASK_FEED_RUN | APS_TASK_EXIT_REQUEST)) == APS_TASK_FEED_RUN)
        {
            s_feed_step();
        }
        xEventGroupSetBits(s_events, APS_TASK_FEED_PARKED);
    }

    xEventGroupSetBits(s_events, APS_TASK_FEED_PARKED | APS_TASK_FEED_EXITED);
    taskENTER_CRITICAL(&s_task_lock);
    s_feed_task = NULL;
    taskEXIT_CRITICAL(&s_task_lock);
    vTaskDelete(NULL);
}

/**
 * @brief AFE fetch Task；drain 完成后报告 DRAIN_DONE，空闲时无限阻塞
 */
static void audio_processor_fetch_task(void *arg)
{
    (void) arg;
    xEventGroupSetBits(s_events, APS_TASK_FETCH_PARKED);
    for (;;)
    {
        const EventBits_t bits =
            xEventGroupWaitBits(s_events, APS_TASK_FETCH_RUN | APS_TASK_EXIT_REQUEST, pdFALSE, pdFALSE, portMAX_DELAY);
        if ((bits & APS_TASK_EXIT_REQUEST) != 0)
        {
            break;
        }

        xEventGroupClearBits(s_events, APS_TASK_FETCH_PARKED);
        while ((xEventGroupGetBits(s_events) & (APS_TASK_FETCH_RUN | APS_TASK_EXIT_REQUEST)) == APS_TASK_FETCH_RUN)
        {
            const bool draining = (xEventGroupGetBits(s_events) & APS_TASK_DRAIN_REQUEST) != 0;
            if (s_fetch_step(draining))
            {
                xEventGroupClearBits(s_events, APS_TASK_FETCH_RUN);
                xEventGroupSetBits(s_events, APS_TASK_DRAIN_DONE);
                break;
            }
        }
        xEventGroupSetBits(s_events, APS_TASK_FETCH_PARKED);
    }

    xEventGroupSetBits(s_events, APS_TASK_FETCH_PARKED | APS_TASK_FETCH_EXITED);
    taskENTER_CRITICAL(&s_task_lock);
    s_fetch_task = NULL;
    taskEXIT_CRITICAL(&s_task_lock);
    vTaskDelete(NULL);
}

esp_err_t audio_processor_task_runtime_init(audio_processor_feed_step_t  feed_step,
                                            audio_processor_fetch_step_t fetch_step)
{
    ESP_RETURN_ON_FALSE(feed_step != NULL && fetch_step != NULL, ESP_ERR_INVALID_ARG, TAG, "AFE Task 回调为空");
    if (s_events != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG, "创建 AFE Task 事件组失败");
    s_feed_step  = feed_step;
    s_fetch_step = fetch_step;
    return ESP_OK;
}

esp_err_t audio_processor_task_runtime_ensure_created(void)
{
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_INVALID_STATE, TAG, "AFE Task Runtime 尚未初始化");
    taskENTER_CRITICAL(&s_task_lock);
    const bool complete = s_feed_task != NULL && s_fetch_task != NULL;
    const bool partial  = (s_feed_task != NULL) != (s_fetch_task != NULL);
    taskEXIT_CRITICAL(&s_task_lock);
    if (complete)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!partial, ESP_ERR_INVALID_STATE, TAG, "AFE Task 仍在收敛退出");

    xEventGroupClearBits(s_events,
                         APS_TASK_EXIT_REQUEST | APS_TASK_FEED_EXITED | APS_TASK_FETCH_EXITED | APS_TASK_FEED_PARKED
                             | APS_TASK_FETCH_PARKED);
    if (xTaskCreate(audio_processor_fetch_task,
                    "aps_fetch",
                    APS_FETCH_TASK_STACK,
                    NULL,
                    APS_FETCH_TASK_PRIO,
                    &s_fetch_task)
        != pdPASS)
    {
        s_fetch_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_processor_feed_task, "aps_feed", APS_FEED_TASK_STACK, NULL, APS_FEED_TASK_PRIO, &s_feed_task)
        != pdPASS)
    {
        s_feed_task = NULL;
        xEventGroupSetBits(s_events, APS_TASK_EXIT_REQUEST | APS_TASK_FETCH_RUN);
        (void) xEventGroupWaitBits(s_events, APS_TASK_FETCH_EXITED, pdFALSE, pdTRUE, pdMS_TO_TICKS(1200));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_processor_task_runtime_begin_processing(void)
{
    xEventGroupClearBits(s_events,
                         APS_TASK_DRAIN_REQUEST | APS_TASK_DRAIN_DONE | APS_TASK_FEED_PARKED | APS_TASK_FETCH_PARKED);
    xEventGroupSetBits(s_events, APS_TASK_FEED_RUN | APS_TASK_FETCH_RUN);
}

void audio_processor_task_runtime_begin_drain(void)
{
    xEventGroupClearBits(s_events, APS_TASK_FEED_RUN | APS_TASK_DRAIN_DONE);
    xEventGroupSetBits(s_events, APS_TASK_DRAIN_REQUEST);
}

esp_err_t audio_processor_task_runtime_park(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "AFE Task 停泊超时无效");
    const EventBits_t parked_mask = created_parked_mask();
    if (parked_mask == 0)
    {
        return ESP_OK;
    }
    xEventGroupClearBits(s_events, APS_TASK_FEED_RUN | APS_TASK_FETCH_RUN | APS_TASK_DRAIN_REQUEST);
    const EventBits_t bits = xEventGroupWaitBits(s_events, parked_mask, pdFALSE, pdTRUE, timeout_ticks(timeout_ms));
    return (bits & parked_mask) == parked_mask ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_processor_task_runtime_wait_drain_and_park(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "AFE drain 超时无效");
    const TickType_t  start_tick = xTaskGetTickCount();
    const TickType_t  total      = timeout_ticks(timeout_ms);
    const EventBits_t drain_bits =
        xEventGroupWaitBits(s_events, APS_TASK_FEED_PARKED | APS_TASK_DRAIN_DONE, pdFALSE, pdTRUE, total);
    if ((drain_bits & (APS_TASK_FEED_PARKED | APS_TASK_DRAIN_DONE)) != (APS_TASK_FEED_PARKED | APS_TASK_DRAIN_DONE))
    {
        xEventGroupClearBits(s_events, APS_TASK_FEED_RUN | APS_TASK_FETCH_RUN | APS_TASK_DRAIN_REQUEST);
        return ESP_ERR_TIMEOUT;
    }

    xEventGroupClearBits(s_events, APS_TASK_FETCH_RUN | APS_TASK_DRAIN_REQUEST);
    const TickType_t  elapsed   = xTaskGetTickCount() - start_tick;
    const TickType_t  remaining = elapsed < total ? total - elapsed : 1;
    const EventBits_t parked    = xEventGroupWaitBits(s_events, APS_TASK_FETCH_PARKED, pdFALSE, pdTRUE, remaining);
    return (parked & APS_TASK_FETCH_PARKED) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_processor_task_runtime_deinit(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "AFE Task 退出超时无效");
    if (s_events == NULL)
    {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_task_lock);
    EventBits_t exit_mask = 0;
    if (s_feed_task != NULL)
    {
        exit_mask |= APS_TASK_FEED_EXITED;
    }
    if (s_fetch_task != NULL)
    {
        exit_mask |= APS_TASK_FETCH_EXITED;
    }
    taskEXIT_CRITICAL(&s_task_lock);

    if (exit_mask != 0)
    {
        xEventGroupClearBits(s_events, exit_mask);
        xEventGroupSetBits(s_events, APS_TASK_EXIT_REQUEST | APS_TASK_FEED_RUN | APS_TASK_FETCH_RUN);
        const EventBits_t bits = xEventGroupWaitBits(s_events, exit_mask, pdFALSE, pdTRUE, timeout_ticks(timeout_ms));
        ESP_RETURN_ON_FALSE((bits & exit_mask) == exit_mask, ESP_ERR_TIMEOUT, TAG, "AFE Task 协作退出超时");
    }

    vEventGroupDelete(s_events);
    s_events     = NULL;
    s_feed_step  = NULL;
    s_fetch_step = NULL;
    return ESP_OK;
}

void audio_processor_task_runtime_get_status(audio_processor_task_status_t *out_status)
{
    if (out_status == NULL)
    {
        return;
    }
    if (s_events == NULL)
    {
        *out_status = (audio_processor_task_status_t) { 0 };
        return;
    }
    const EventBits_t bits = xEventGroupGetBits(s_events);
    taskENTER_CRITICAL(&s_task_lock);
    const bool feed_created  = s_feed_task != NULL;
    const bool fetch_created = s_fetch_task != NULL;
    taskEXIT_CRITICAL(&s_task_lock);
    *out_status = (audio_processor_task_status_t) {
        .tasks_created = feed_created || fetch_created,
        .feed_parked   = !feed_created || (bits & APS_TASK_FEED_PARKED) != 0,
        .fetch_parked  = !fetch_created || (bits & APS_TASK_FETCH_PARKED) != 0,
    };
}
