/**
 * @file system_periodic_timer.c
 * @brief 使用 ESP Timer 实现周期定时执行抽象
 */
#include "system_periodic_timer.h"

#include <limits.h>
#include <stdlib.h>

#include "esp_timer.h"

/** @brief System 持有的周期定时器运行时 */
struct system_periodic_timer
{
    esp_timer_handle_t               timer;
    system_periodic_timer_callback_t callback;
    void                            *context;
    int64_t                          last_dispatch_us;
    bool                             running;
};

/** @brief 隔离 ESP Timer 回调类型并转发到稳定 System 回调 */
static void system_periodic_timer_dispatch(void *context)
{
    struct system_periodic_timer *timer = context;
    const int64_t now_us     = esp_timer_get_time();
    const int64_t elapsed_us = now_us - timer->last_dispatch_us;
    timer->last_dispatch_us  = now_us;

    uint64_t elapsed_ms = elapsed_us > 0 ? ((uint64_t) elapsed_us + 999ULL) / 1000ULL : 1ULL;
    if (elapsed_ms > UINT32_MAX)
    {
        elapsed_ms = UINT32_MAX;
    }
    timer->callback((uint32_t) elapsed_ms, timer->context);
}

esp_err_t system_periodic_timer_create_borrow(const system_periodic_timer_config_t *config,
                                              system_periodic_timer_handle_t       *out_timer)
{
    if (config == NULL || out_timer == NULL || config->name == NULL || config->name[0] == '\0'
        || config->callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct system_periodic_timer *timer = calloc(1U, sizeof(*timer));
    if (timer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    timer->callback = config->callback;
    timer->context  = config->context;

    const esp_timer_create_args_t timer_args = {
        .callback              = system_periodic_timer_dispatch,
        .arg                   = timer,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = config->name,
        .skip_unhandled_events = config->skip_unhandled_events,
    };
    esp_err_t error = esp_timer_create(&timer_args, &timer->timer);
    if (error != ESP_OK)
    {
        free(timer);
        return error;
    }

    *out_timer = timer;
    return ESP_OK;
}

esp_err_t system_periodic_timer_start(system_periodic_timer_handle_t timer, uint32_t period_ms)
{
    if (timer == NULL || period_ms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (timer->running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    timer->last_dispatch_us = esp_timer_get_time();
    esp_err_t error = esp_timer_start_periodic(timer->timer, (uint64_t) period_ms * 1000ULL);
    if (error == ESP_OK)
    {
        timer->running = true;
    }
    return error;
}

esp_err_t system_periodic_timer_stop(system_periodic_timer_handle_t timer)
{
    if (timer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!timer->running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = esp_timer_stop(timer->timer);
    if (error == ESP_OK)
    {
        timer->running = false;
    }
    return error;
}

esp_err_t system_periodic_timer_destroy(system_periodic_timer_handle_t timer)
{
    if (timer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (timer->running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = esp_timer_delete(timer->timer);
    if (error == ESP_OK)
    {
        free(timer);
    }
    return error;
}
