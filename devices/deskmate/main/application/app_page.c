/*
 * 文件职责：维护 App 当前页面，提供默认环形导航与交互页输入分发。
 */
#include "app_page.h"

#include "app_ota.h"
#include "app_pomodoro.h"
#include "app_settings.h"
#include "app_voice.h"
#include "status_bar_presenter.h"
#include "presentation_dispatch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#define APP_PAGE_TRANSITION_TIMEOUT_US (500LL * 1000LL)

static const char            *TAG                 = "app_page";
static portMUX_TYPE           s_page_lock         = portMUX_INITIALIZER_UNLOCKED;
static presentation_page_id_t s_current_page      = PRESENTATION_PAGE_HOME;
static presentation_page_id_t s_transition_target = PRESENTATION_PAGE_HOME;
static bool                   s_transitioning;
static int64_t                s_transition_deadline_us;

esp_err_t app_page_set_current(presentation_page_id_t page)
{
    if ((unsigned) page >= PRESENTATION_PAGE_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_page_lock);
    s_current_page = page;
    taskEXIT_CRITICAL(&s_page_lock);
    return ESP_OK;
}

presentation_page_id_t app_page_get_current(void)
{
    taskENTER_CRITICAL(&s_page_lock);
    const presentation_page_id_t page = s_current_page;
    taskEXIT_CRITICAL(&s_page_lock);
    return page;
}

esp_err_t app_page_next(void)
{
    const presentation_page_id_t current = app_page_get_current();
    const presentation_page_id_t next    = (presentation_page_id_t) ((current + 1) % PRESENTATION_PAGE_COUNT);
    return app_page_show(next, PRESENTATION_NAV_DIR_FORWARD);
}

esp_err_t app_page_prev(void)
{
    const presentation_page_id_t current = app_page_get_current();
    const presentation_page_id_t prev =
        current == PRESENTATION_PAGE_HOME ? (PRESENTATION_PAGE_COUNT - 1) : (current - 1);
    return app_page_show(prev, PRESENTATION_NAV_DIR_BACKWARD);
}

esp_err_t app_page_dispatch_current(presentation_nav_dir_t dir)
{
    return app_page_show(app_page_get_current(), dir);
}

esp_err_t app_page_show(presentation_page_id_t page, presentation_nav_dir_t dir)
{
    if ((unsigned) page >= PRESENTATION_PAGE_COUNT || (unsigned) dir > PRESENTATION_NAV_DIR_BACKWARD)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const presentation_page_id_t previous = app_page_get_current();
    if (previous == PRESENTATION_PAGE_SETTINGS && page != PRESENTATION_PAGE_SETTINGS)
    {
        ESP_RETURN_ON_FALSE(!app_ota_is_navigation_locked(),
                            ESP_ERR_INVALID_STATE,
                            TAG,
                            "OTA 事务执行期间禁止离开设置页");
        ESP_RETURN_ON_ERROR(app_settings_reset(), TAG, "离开设置页前清理设置会话失败");
    }

    const esp_err_t err = presentation_dispatch_page_switch(page, dir);
    if (err != ESP_OK)
    {
        return err;
    }

    taskENTER_CRITICAL(&s_page_lock);
    s_current_page = page;
    if (page != previous)
    {
        s_transitioning          = true;
        s_transition_target      = page;
        s_transition_deadline_us = esp_timer_get_time() + APP_PAGE_TRANSITION_TIMEOUT_US;
    }
    else if (!s_transitioning)
    {
        s_transition_deadline_us = 0;
    }
    taskEXIT_CRITICAL(&s_page_lock);

    (void) status_bar_presenter_set_page(page);
    return ESP_OK;
}

esp_err_t app_page_publish_initial_ui(void)
{
    ESP_RETURN_ON_ERROR(app_page_show(app_page_get_current(), PRESENTATION_NAV_DIR_NONE), TAG, "显示初始页面失败");
    return presentation_dispatch_status_update();
}

bool app_page_is_transitioning(void)
{
    bool expired = false;
    taskENTER_CRITICAL(&s_page_lock);
    if (s_transitioning && esp_timer_get_time() >= s_transition_deadline_us)
    {
        s_transitioning          = false;
        s_transition_deadline_us = 0;
        expired                  = true;
    }
    const bool transitioning = s_transitioning;
    taskEXIT_CRITICAL(&s_page_lock);
    if (expired)
    {
        ESP_LOGW(TAG, "Screen 加载完成通知超时，已解除按键门控");
    }
    return transitioning;
}

void app_page_notify_screen_loaded(presentation_page_id_t page)
{
    taskENTER_CRITICAL(&s_page_lock);
    if (s_transitioning && page == s_transition_target)
    {
        s_transitioning          = false;
        s_transition_deadline_us = 0;
    }
    taskEXIT_CRITICAL(&s_page_lock);
}

bool app_page_consume_input(device_button_event_t key_event)
{
    /* 交互页优先处理，消费了就不走默认导航 */
    switch (app_page_get_current())
    {
        case PRESENTATION_PAGE_POMODORO:
            if (app_pomodoro_consume_input(key_event))
            {
                return true;
            }
            break;
        case PRESENTATION_PAGE_VOICE:
            if (app_voice_consume_input(key_event))
            {
                return true;
            }
            break;
        case PRESENTATION_PAGE_SETTINGS:
            if (app_settings_consume_input(key_event))
            {
                return true;
            }
            break;
        default:
            break;
    }

    /* 没被消费 → 默认环形导航 */
    switch (key_event)
    {
        case DEVICE_BUTTON_EVENT_LEFT_SHORT:
            return app_page_prev() == ESP_OK;
        case DEVICE_BUTTON_EVENT_RIGHT_SHORT:
            return app_page_next() == ESP_OK;
        default:
            return false;
    }
}
