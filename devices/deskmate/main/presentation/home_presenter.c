/*
 * 文件职责：聚合时间、天气、环境、下一日程和未读邮件事实并维护首页 View Model。
 */
#include "home_presenter.h"

#include <string.h>
#include <time.h>

#include "calendar_presenter.h"
#include "mail_presenter.h"
#include "presentation_dispatch.h"
#include "weather_presenter.h"
#include "environment_service.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "system_clock.h"

static const char                   *TAG = "home_presenter";
static home_environment_view_model_t s_env_view;

/**
 * @brief 将当前系统时间同步转换为首页 View
 */
static void load_time_view(home_time_view_model_t *out)
{
    memset(out, 0, sizeof(*out));
    out->status = PRESENTATION_DATA_EMPTY;

    system_clock_snapshot_t snapshot;
    if (system_clock_get_snapshot_copy(&snapshot) != ESP_OK || !snapshot.valid)
    {
        return;
    }
    struct tm local_time;
    if (localtime_r(&snapshot.utc_timestamp, &local_time) == NULL)
    {
        return;
    }
    out->year   = (uint16_t) (local_time.tm_year + 1900);
    out->month  = (uint8_t) (local_time.tm_mon + 1);
    out->day    = (uint8_t) local_time.tm_mday;
    out->hour   = (uint8_t) local_time.tm_hour;
    out->minute = (uint8_t) local_time.tm_min;
    out->status = PRESENTATION_DATA_OK;
}

/**
 * @brief 从日历和邮箱页 View Model 裁剪首页摘要
 *
 * 下一日程保持日历列表的既有排序并选择首条事件；业务列表有效但为空时保留 OK 状态，
 * 由 has_next_event 明确表达没有可展示事件。
 *
 * @param[out] out_view 接收首页摘要字段
 */
static void load_dashboard_summary(home_view_model_t *out_view)
{
    calendar_view_model_t calendar = { 0 };
    calendar_presenter_get_view_copy(&calendar);
    out_view->next_event_status = calendar.status;
    if ((calendar.status == PRESENTATION_DATA_OK || calendar.status == PRESENTATION_DATA_STALE)
        && calendar.event_count > 0)
    {
        out_view->next_event     = calendar.events[0];
        out_view->has_next_event = true;
    }

    mail_view_model_t mail = { 0 };
    mail_presenter_get_view_copy(&mail);
    out_view->unread_mail_status = mail.status;
    out_view->unread_mail_count  = mail.unread_count;
}

/**
 * @brief 收到时间校准事件后请求 UI 重新拉取 View Model
 */
static void on_time_event(system_clock_event_t event, const system_clock_snapshot_t *snapshot, void *ctx)
{
    (void) event;
    (void) snapshot;
    (void) ctx;

    (void) presentation_dispatch_status_update();
}

/**
 * @brief 收到环境 Service 通知后拉取联合快照并更新首页 View
 */
static void on_environment_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) id;
    (void) data;

    environment_service_snapshot_t snapshot;
    const esp_err_t                error = environment_service_get_snapshot_copy(&snapshot);
    if (error != ESP_OK)
    {
        s_env_view.status = PRESENTATION_DATA_ERROR;
    }
    else
    {
        s_env_view.temperature_centi = snapshot.environment.temperature_centi;
        s_env_view.humidity_centi    = snapshot.environment.humidity_centi;
        s_env_view.status            = snapshot.environment.valid && snapshot.environment.last_error == ESP_OK
                                           ? PRESENTATION_DATA_OK
                                           : PRESENTATION_DATA_ERROR;
    }
    (void) presentation_dispatch_status_update();
}

esp_err_t home_presenter_init(void)
{
    memset(&s_env_view, 0, sizeof(s_env_view));
    s_env_view.status = PRESENTATION_DATA_EMPTY;
    ESP_RETURN_ON_ERROR(system_clock_register_callback_borrow(on_time_event, NULL), TAG, "注册首页时间回调失败");
    const esp_err_t environment_error = esp_event_handler_register(ENVIRONMENT_SERVICE_EVENT,
                                                                   ENVIRONMENT_SERVICE_EVENT_ENVIRONMENT_UPDATED,
                                                                   on_environment_event,
                                                                   NULL);
    if (environment_error != ESP_OK)
    {
        (void) system_clock_unregister_callback(on_time_event, NULL);
    }
    return environment_error;
}

void home_presenter_get_view_copy(home_view_model_t *out_view)
{
    if (out_view == NULL)
    {
        return;
    }
    memset(out_view, 0, sizeof(*out_view));
    load_time_view(&out_view->time);
    out_view->env = s_env_view;
    weather_presenter_get_view_copy(&out_view->weather);
    load_dashboard_summary(out_view);
}
