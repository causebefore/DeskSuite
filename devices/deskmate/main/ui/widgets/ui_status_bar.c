/*
 * 文件职责：实现顶部常驻状态栏，显示页面名、时间、Wi-Fi 图标和电池信息。
 * 主要依赖：LVGL、ui_platform 字体接口、status_bar_presenter 视图数据。
 * 调用方：ui_main、UI_MSG_STATUS_UPDATE 处理流程。
 *
 * 布局（400×32, 白底黑字）：
 *   左=页面名  中=时间(HH:MM)  右=WiFi图标+电池百分比+电池图标
 *   底部双线分隔（1px+2px间距+1px）
 */
#include "ui_status_bar.h"

#include <string.h>

#include "esp_log.h"
#include "status_icon_resolver.h"
#include "ui_common.h"
#include "ui_main.h"

#include <stdio.h>

static const char *TAG = "ui_status_bar";

/* ── 布局常量 ── */
#define STATUS_BAR_W     400
#define STATUS_BAR_H     32
#define STATUS_BAR_PAD_X 12

/* ── 控件句柄 ── */
static lv_obj_t               *s_page_label;
static lv_obj_t               *s_time_label;
static lv_obj_t               *s_wifi_icon;
static lv_obj_t               *s_server_icon;
static lv_obj_t               *s_batt_pct;
static lv_obj_t               *s_batt_icon;
static lv_timer_t             *s_wifi_blink_timer;
static bool                    s_created       = false;
static bool                    s_status_cached = false;
static status_bar_view_model_t s_last_status;

/**
 * @brief 根据电池有效性和百分比选择对应档位的电量图标
 *
 * 数据无效或电量≤5% 视为空电量；此后按 25/50/75 三档递进。
 *
 * @param valid   电池数据是否有效
 * @param percent 电池百分比（0-100）
 * @return status_icon_id_t 对应档位的电池图标 ID
 */
static status_icon_id_t battery_icon_for(bool valid, uint8_t percent)
{
    if (!valid || percent <= 5)
    {
        return STATUS_ICON_BATTERY_0;
    }
    if (percent <= 25)
    {
        return STATUS_ICON_BATTERY_25;
    }
    if (percent <= 50)
    {
        return STATUS_ICON_BATTERY_50;
    }
    if (percent <= 75)
    {
        return STATUS_ICON_BATTERY_75;
    }
    return STATUS_ICON_BATTERY_100;
}

/** @brief Wi-Fi 连接中图标闪烁周期（毫秒） */
#define WIFI_BLINK_PERIOD_MS 500

/**
 * @brief Wi-Fi 连接中闪烁定时器：在"无图标"与"在线图标"之间切换
 *
 * 每次触发顺带重拉一次状态栏 View Model，保证网络事实及时收敛。
 */
static void wifi_blink_timer_cb(lv_timer_t *timer)
{
    (void) timer;
    if (s_wifi_icon == NULL)
    {
        return;
    }

    status_bar_view_model_t view;
    if (status_bar_presenter_get_view_copy(&view) != ESP_OK)
    {
        return;
    }
    (void) ui_status_bar_update(&view);

    if (!view.wifi_connecting)
    {
        lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (lv_obj_has_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 创建状态栏的全部子控件并初始化句柄
 *
 * 已创建过时直接返回（幂等）。依次建立页面名、时间、Wi-Fi 图标、
 * 服务端图标、电池百分比、电池图标，以及底部双线分隔。
 *
 * @param parent 状态栏父容器
 */
static void create_controls(lv_obj_t *parent)
{
    if (s_created)
    {
        return;
    }

    /* ── 左侧：页面名 ── */
    s_page_label = ui_common_new_text16_semibold(parent);
    lv_obj_set_pos(s_page_label, STATUS_BAR_PAD_X, 7);
    lv_obj_set_size(s_page_label, 80, 18);
    lv_label_set_long_mode(s_page_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_page_label, "首页");

    /* ── 中间：时间 ── */
    s_time_label = ui_common_new_text16_regular(parent);
    lv_obj_set_style_text_letter_space(s_time_label, 1, 0);
    lv_obj_set_pos(s_time_label, 164, 7);
    lv_obj_set_size(s_time_label, 72, 18);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_time_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_time_label, "--:--");

    /* ── 右侧：WiFi 图标 ── */
    s_wifi_icon = lv_image_create(parent);
    lv_image_set_src(s_wifi_icon, status_icon_resolver_get(STATUS_ICON_WIFI_OFFLINE));
    lv_obj_set_size(s_wifi_icon, 20, 20);
    lv_obj_set_pos(s_wifi_icon, 282, 4);

    /* 右侧：服务端图标仅有在线、离线两态。 */
    s_server_icon = lv_image_create(parent);
    lv_image_set_src(s_server_icon, status_icon_resolver_get(STATUS_ICON_SERVER_OFFLINE));
    lv_obj_set_size(s_server_icon, 20, 20);
    lv_obj_set_pos(s_server_icon, 306, 4);

    /* ── 右侧：电池百分比 ── */
    s_batt_pct = ui_common_new_text16_regular(parent);
    lv_obj_set_pos(s_batt_pct, 330, 7);
    lv_obj_set_size(s_batt_pct, 36, 18);
    lv_obj_set_style_text_align(s_batt_pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_batt_pct, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_batt_pct, "--");

    /* 电池图标由五档 SVG 转换数组提供。 */
    s_batt_icon = lv_image_create(parent);
    lv_image_set_src(s_batt_icon, status_icon_resolver_get(STATUS_ICON_BATTERY_0));
    lv_obj_set_size(s_batt_icon, 20, 20);
    lv_obj_set_pos(s_batt_icon, 370, 4);

    /* ── 底部双线分隔：1px + 2px 间距 + 1px ── */
    (void) ui_common_new_rule(parent, 0, 27, STATUS_BAR_W, UI_RULE_THIN);
    (void) ui_common_new_rule(parent, 0, 30, STATUS_BAR_W, UI_RULE_THIN);

    s_wifi_blink_timer = lv_timer_create(wifi_blink_timer_cb, WIFI_BLINK_PERIOD_MS, NULL);
    if (s_wifi_blink_timer == NULL)
    {
        ESP_LOGW(TAG, "Wi-Fi 闪烁定时器创建失败");
    }

    s_created = true;
    ESP_LOGI(TAG, "状态栏控件创建完成");
}

esp_err_t ui_status_bar_init(void)
{
    lv_obj_t *parent = ui_main_get_status_bar();
    if (parent == NULL)
    {
        ESP_LOGE(TAG, "状态栏容器为空");
        return ESP_ERR_INVALID_STATE;
    }

    create_controls(parent);
    return ESP_OK;
}

/**
 * @brief 清空随状态栏控件树失效的句柄和快照缓存
 */
void ui_status_bar_deinit(void)
{
    if (s_wifi_blink_timer != NULL)
    {
        lv_timer_delete(s_wifi_blink_timer);
        s_wifi_blink_timer = NULL;
    }
    s_page_label    = NULL;
    s_time_label    = NULL;
    s_wifi_icon     = NULL;
    s_server_icon   = NULL;
    s_batt_pct      = NULL;
    s_batt_icon     = NULL;
    s_created       = false;
    s_status_cached = false;
    memset(&s_last_status, 0, sizeof(s_last_status));
}

esp_err_t ui_status_bar_update(const status_bar_view_model_t *status)
{
    if (s_page_label == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (status->wifi_connecting)
    {
        /* 连接中固定使用在线图标，可见性由闪烁定时器控制 */
        lv_image_set_src(s_wifi_icon, status_icon_resolver_get(STATUS_ICON_WIFI_ONLINE));
    }
    else if (!s_status_cached || s_last_status.wifi_connected != status->wifi_connected
             || s_last_status.wifi_connecting != status->wifi_connecting)
    {
        lv_image_set_src(
            s_wifi_icon,
            status_icon_resolver_get(status->wifi_connected ? STATUS_ICON_WIFI_ONLINE : STATUS_ICON_WIFI_OFFLINE));
    }

    if (!s_status_cached || s_last_status.server_online != status->server_online)
    {
        lv_image_set_src(
            s_server_icon,
            status_icon_resolver_get(status->server_online ? STATUS_ICON_SERVER_ONLINE : STATUS_ICON_SERVER_OFFLINE));
    }

    if (!s_status_cached || strcmp(s_last_status.page_title, status->page_title) != 0)
    {
        lv_label_set_text(s_page_label, status->page_title);
    }

    if (!s_status_cached || s_last_status.battery_valid != status->battery_valid
        || s_last_status.battery_percent != status->battery_percent)
    {
        char pct_str[8];
        if (status->battery_valid)
        {
            snprintf(pct_str, sizeof(pct_str), "%u%%", (unsigned) status->battery_percent);
        }
        else
        {
            snprintf(pct_str, sizeof(pct_str), "--");
        }
        lv_label_set_text(s_batt_pct, pct_str);

        lv_image_set_src(s_batt_icon,
                         status_icon_resolver_get(battery_icon_for(status->battery_valid, status->battery_percent)));
    }

    if (!s_status_cached || s_last_status.time_valid != status->time_valid || s_last_status.hour != status->hour
        || s_last_status.minute != status->minute)
    {
        char time_str[8];
        if (status->time_valid)
        {
            snprintf(time_str, sizeof(time_str), "%02u:%02u", (unsigned) status->hour, (unsigned) status->minute);
        }
        else
        {
            snprintf(time_str, sizeof(time_str), "--:--");
        }
        lv_label_set_text(s_time_label, time_str);
    }

    s_last_status   = *status;
    s_status_cached = true;

    return ESP_OK;
}
