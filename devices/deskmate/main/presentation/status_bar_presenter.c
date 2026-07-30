/**
 * @file status_bar_presenter.c
 * @brief 将页面、网络、电量和时间事实聚合为顶部状态栏 View Model
 */

#include "status_bar_presenter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "presentation_dispatch.h"
#include "environment_service.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "network_manager.h"
#include "system_clock.h"

static const char             *TAG = "status_bar_presenter";
static status_bar_view_model_t s_status_bar_view;
static portMUX_TYPE            s_status_bar_lock = portMUX_INITIALIZER_UNLOCKED;

static void notify_if_changed(bool changed)
{
    if (!changed)
    {
        return;
    }

    esp_err_t err = presentation_dispatch_status_bar_update();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "投递状态栏更新失败: %s", esp_err_to_name(err));
    }
}

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0)
    {
        return;
    }

    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_len, "%s", src);
}

static const char *page_id_to_title(presentation_page_id_t page)
{
    switch (page)
    {
        case PRESENTATION_PAGE_HOME:
            return "首页";
        case PRESENTATION_PAGE_POMODORO:
            return "番茄钟";
        case PRESENTATION_PAGE_WEATHER:
            return "天气";
        case PRESENTATION_PAGE_VOICE:
            return "语音";
        case PRESENTATION_PAGE_CALENDAR:
            return "日历";
        case PRESENTATION_PAGE_MAIL:
            return "邮箱";
        case PRESENTATION_PAGE_QUOTA:
            return "限额";
        case PRESENTATION_PAGE_SETTINGS:
            return "设置";
        default:
            return "首页";
    }
}

/** @brief 在状态栏锁内更新页面标题 */
static bool update_page_title_locked(presentation_page_id_t page)
{
    const char *title = page_id_to_title(page);
    if (strcmp(s_status_bar_view.page_title, title) == 0)
    {
        return false;
    }

    copy_text(s_status_bar_view.page_title, sizeof(s_status_bar_view.page_title), title);
    return true;
}

/**
 * @brief 从 Communication 只读快照查询 Wi-Fi 连接与连接中状态
 *
 * @param[out] out_connected true 表示 STA 已关联、取得 IPv4 且 Network Manager 在线
 * @param[out] out_connecting true 表示 Network Manager 正在连接、退避重试或校验连通性
 */
static void query_network_view(bool *out_connected, bool *out_connecting)
{
    network_manager_status_t status    = { 0 };
    connect_link_info_t      link      = { 0 };
    const bool               status_ok = network_manager_get_status_copy(&status) == ESP_OK;
    const bool connected  = status_ok && status.state == NETWORK_STATE_ONLINE
                            && connect_get_link_snapshot_copy(&link) == ESP_OK && link.associated && link.has_ipv4;
    const bool connecting = !connected && status_ok
                            && (status.state == NETWORK_STATE_CONNECTING || status.state == NETWORK_STATE_RETRY_WAIT
                                || status.state == NETWORK_STATE_VALIDATING);
    *out_connected        = connected;
    *out_connecting       = connecting;
}

/** @brief 在状态栏锁内更新电池事实 */
static bool update_battery_view_locked(const environment_service_battery_snapshot_t *snap)
{
    if (snap == NULL)
    {
        return false;
    }

    const bool    valid   = snap->valid;
    const uint8_t percent = snap->percent > 100 ? 100 : snap->percent;

    if (s_status_bar_view.battery_valid == valid && s_status_bar_view.battery_percent == percent)
    {
        return false;
    }

    s_status_bar_view.battery_valid   = valid;
    s_status_bar_view.battery_percent = percent;
    return true;
}

/**
 * @brief 在锁外把系统时钟快照转换为状态栏时间字段
 *
 * @param[in] snap 系统时钟快照
 * @param[out] out_valid 时间是否有效
 * @param[out] out_hour 小时输出
 * @param[out] out_minute 分钟输出
 */
static void resolve_time_view(const system_clock_snapshot_t *snap, bool *out_valid, uint8_t *out_hour,
                              uint8_t *out_minute)
{
    bool    valid  = snap != NULL && snap->valid;
    uint8_t hour   = 0U;
    uint8_t minute = 0U;
    if (valid)
    {
        struct tm local_time;
        if (localtime_r(&snap->utc_timestamp, &local_time) == NULL)
        {
            valid = false;
        }
        else
        {
            hour   = (uint8_t) local_time.tm_hour;
            minute = (uint8_t) local_time.tm_min;
        }
    }
    *out_valid  = valid;
    *out_hour   = hour;
    *out_minute = minute;
}

/** @brief 在状态栏锁内更新时间事实 */
static bool update_time_view_locked(bool valid, uint8_t hour, uint8_t minute)
{
    if (s_status_bar_view.time_valid == valid && s_status_bar_view.hour == (valid ? hour : 0)
        && s_status_bar_view.minute == (valid ? minute : 0))
    {
        return false;
    }

    s_status_bar_view.time_valid = valid;
    s_status_bar_view.hour       = valid ? hour : 0;
    s_status_bar_view.minute     = valid ? minute : 0;
    return true;
}

/** @brief 在状态栏锁内更新服务端可达事实 */
static bool update_server_view_locked(bool online)
{
    if (s_status_bar_view.server_online == online)
    {
        return false;
    }
    s_status_bar_view.server_online = online;
    return true;
}

/** @brief 在状态栏锁内更新番茄钟角标事实 */
static bool update_pomodoro_view_locked(status_bar_pomodoro_state_t state, uint16_t remaining_minutes)
{
    const uint16_t minutes = state == STATUS_BAR_POMODORO_RUNNING ? remaining_minutes : 0U;
    if (s_status_bar_view.pomodoro_state == state && s_status_bar_view.pomodoro_minutes == minutes)
    {
        return false;
    }
    s_status_bar_view.pomodoro_state   = state;
    s_status_bar_view.pomodoro_minutes = minutes;
    return true;
}

static void on_battery_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) id;
    (void) data;

    environment_service_snapshot_t snapshot;
    if (environment_service_get_snapshot_copy(&snapshot) == ESP_OK)
    {
        taskENTER_CRITICAL(&s_status_bar_lock);
        const bool changed = update_battery_view_locked(&snapshot.battery);
        taskEXIT_CRITICAL(&s_status_bar_lock);
        notify_if_changed(changed);
    }
}

static void on_time_event(system_clock_event_t event, const system_clock_snapshot_t *snapshot, void *ctx)
{
    (void) event;
    (void) snapshot;
    (void) ctx;

    notify_if_changed(true);
}

esp_err_t status_bar_presenter_init(void)
{
    taskENTER_CRITICAL(&s_status_bar_lock);
    memset(&s_status_bar_view, 0, sizeof(s_status_bar_view));
    copy_text(s_status_bar_view.page_title, sizeof(s_status_bar_view.page_title), "首页");
    taskEXIT_CRITICAL(&s_status_bar_lock);
    esp_err_t error = esp_event_handler_register(ENVIRONMENT_SERVICE_EVENT,
                                                 ENVIRONMENT_SERVICE_EVENT_BATTERY_UPDATED,
                                                 on_battery_event,
                                                 NULL);
    ESP_RETURN_ON_ERROR(error, TAG, "注册状态栏电池事件失败");
    error = system_clock_register_callback_borrow(on_time_event, NULL);
    if (error != ESP_OK)
    {
        (void) esp_event_handler_unregister(ENVIRONMENT_SERVICE_EVENT,
                                            ENVIRONMENT_SERVICE_EVENT_BATTERY_UPDATED,
                                            on_battery_event);
    }
    return error;
}

esp_err_t status_bar_presenter_get_view_copy(status_bar_view_model_t *out_view)
{
    if (out_view == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    system_clock_snapshot_t snapshot          = { 0 };
    const bool              has_time_snapshot = system_clock_get_snapshot_copy(&snapshot) == ESP_OK;
    bool                    time_valid        = false;
    uint8_t                 hour              = 0U;
    uint8_t                 minute            = 0U;
    if (has_time_snapshot)
    {
        resolve_time_view(&snapshot, &time_valid, &hour, &minute);
    }
    bool network_connected  = false;
    bool network_connecting = false;
    query_network_view(&network_connected, &network_connecting);

    taskENTER_CRITICAL(&s_status_bar_lock);
    if (has_time_snapshot)
    {
        (void) update_time_view_locked(time_valid, hour, minute);
    }
    s_status_bar_view.wifi_connected  = network_connected;
    s_status_bar_view.wifi_connecting = network_connecting;
    *out_view                         = s_status_bar_view;
    taskEXIT_CRITICAL(&s_status_bar_lock);
    return ESP_OK;
}

bool status_bar_presenter_set_page(presentation_page_id_t page)
{
    taskENTER_CRITICAL(&s_status_bar_lock);
    const bool changed = update_page_title_locked(page);
    taskEXIT_CRITICAL(&s_status_bar_lock);
    notify_if_changed(changed);
    return changed;
}

void status_bar_presenter_set_server_online(bool online)
{
    taskENTER_CRITICAL(&s_status_bar_lock);
    const bool changed = update_server_view_locked(online);
    taskEXIT_CRITICAL(&s_status_bar_lock);
    notify_if_changed(changed);
}

void status_bar_presenter_set_pomodoro(status_bar_pomodoro_state_t state, uint16_t remaining_minutes)
{
    if ((unsigned) state > STATUS_BAR_POMODORO_DONE)
    {
        return;
    }
    taskENTER_CRITICAL(&s_status_bar_lock);
    const bool changed = update_pomodoro_view_locked(state, remaining_minutes);
    taskEXIT_CRITICAL(&s_status_bar_lock);
    /*
     * 番茄钟 Application 会在同一串行路径发布专用刷新事件。此处不重复投递，避免
     * 每秒产生两条状态栏消息；其他状态栏事实仍沿用 notify_if_changed()。
     */
    (void) changed;
}
