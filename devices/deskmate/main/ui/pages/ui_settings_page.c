/*
 * 文件职责：实现设置概览以及由 lv_menu、lv_group 管理的两层设置菜单。
 * 主要依赖：settings/system/ota/web_file Presenter、UI Runtime 用户意图。
 * 调用方：ui_router、ui_main。
 */
#include "ui_settings_page.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_presenter.h"
#include "pomodoro_presenter.h"
#include "sdkconfig.h"
#include "settings_presenter.h"
#include "ui_common.h"
#include "ui_format.h"
#include "ui_platform_font.h"
#include "ui_runtime.h"
#include "ui_system_page.h"
#include "web_file_presenter.h"

typedef enum
{
    SETTINGS_LOCATION_OVERVIEW = 0,
    SETTINGS_LOCATION_ROOT,
    SETTINGS_LOCATION_NETWORK,
    SETTINGS_LOCATION_WEB_FILE,
    SETTINGS_LOCATION_SYSTEM,
    SETTINGS_LOCATION_OTA,
    SETTINGS_LOCATION_POMODORO,
} settings_location_t;

typedef enum
{
    SETTINGS_ITEM_NETWORK = 0,
    SETTINGS_ITEM_WEB_FILE,
    SETTINGS_ITEM_SYSTEM,
    SETTINGS_ITEM_OTA,
    SETTINGS_ITEM_POMODORO,
    SETTINGS_ITEM_COUNT,
} settings_item_t;

typedef struct
{
    lv_obj_t *parent;

    lv_obj_t *overview_network;
    lv_obj_t *overview_version;

    lv_obj_t *menu;
    lv_obj_t *root_page;
    lv_obj_t *network_page;
    lv_obj_t *web_file_page;
    lv_obj_t *system_page;
    lv_obj_t *ota_page;
    lv_obj_t *pomodoro_page;
    lv_obj_t *network_body;
    lv_obj_t *web_file_body;
    lv_obj_t *system_body;
    lv_obj_t *ota_body;
    lv_obj_t *pomodoro_body;
    lv_obj_t *root_items[SETTINGS_ITEM_COUNT];

    lv_group_t *group;
    lv_timer_t *result_timer;
    lv_obj_t   *return_focus;
    lv_obj_t   *child_action;

    settings_location_t location;
    int                 rendered_ota_state;
    bool                web_file_exit_pending;
    uint8_t             pomodoro_selected;
    bool                pomodoro_editing;
    ui_pomodoro_settings_intent_t pomodoro_draft;
} settings_ui_state_t;

static settings_ui_state_t s_view = {
    .location           = SETTINGS_LOCATION_OVERVIEW,
    .rendered_ota_state = -1,
};

static void return_to_root(void);
static void on_network_action_clicked(lv_event_t *event);
static void on_ota_install_clicked(lv_event_t *event);
static void on_web_file_retry_clicked(lv_event_t *event);
static void render_pomodoro(esp_err_t action_error);

/** @brief 创建并定位一个 16 像素正文标签 */
static lv_obj_t *new_text16(lv_obj_t *parent, const char *text, int32_t x, int32_t y, int32_t width, int32_t height)
{
    lv_obj_t *label = ui_common_new_text16_regular(parent);
    ui_common_set_label(label, text, x, y, width, height, LV_TEXT_ALIGN_LEFT);
    return label;
}

/** @brief 创建并定位一个 24 像素标题标签 */
static lv_obj_t *new_text24(lv_obj_t *parent, const char *text, int32_t x, int32_t y, int32_t width, int32_t height)
{
    lv_obj_t *label = ui_common_new_text24_semibold(parent);
    ui_common_set_label(label, text, x, y, width, height, LV_TEXT_ALIGN_LEFT);
    return label;
}

/** @brief 把网络阶段转换为设置页用户可读状态 */
static const char *network_state_text(const settings_view_model_t *view)
{
    switch (view->network_state)
    {
        case SETTINGS_NETWORK_VIEW_CONNECTED:
            return "已连接";
        case SETTINGS_NETWORK_VIEW_CONNECTING:
            return "连接中";
        case SETTINGS_NETWORK_VIEW_PORTAL:
            return "配网中";
        case SETTINGS_NETWORK_VIEW_FAILED:
            return "连接失败";
        case SETTINGS_NETWORK_VIEW_IDLE:
        default:
            return "未连接";
    }
}

/** @brief 更新设置概览的网络与版本摘要 */
static void populate_overview(void)
{
    settings_view_model_t view;
    settings_presenter_get_view_copy(&view);

    char text[96];
    if (view.wifi_connected && view.station_ssid[0] != '\0')
    {
        (void) snprintf(text, sizeof(text), "网络  已连接 · %s", view.station_ssid);
    }
    else
    {
        (void) snprintf(text, sizeof(text), "网络  %s", network_state_text(&view));
    }
    lv_label_set_text(s_view.overview_network, text);

    (void) snprintf(text, sizeof(text), "版本  %s", view.current_version[0] != '\0' ? view.current_version : "--");
    lv_label_set_text(s_view.overview_version, text);
}

/** @brief 创建顶层设置概览；该视图不含菜单交互状态 */
static void show_overview(void)
{
    lv_obj_clean(s_view.parent);
    (void) new_text24(s_view.parent, "设置概览", 16, 18, 360, 30);
    s_view.overview_network = new_text16(s_view.parent, "", 20, 78, 360, 22);
    s_view.overview_version = new_text16(s_view.parent, "", 20, 116, 360, 22);
    (void) ui_common_new_rule(s_view.parent, 16, 166, 368, UI_RULE_THIN);
    (void) new_text16(s_view.parent, "长按右键进入设置", 20, 190, 360, 22);
    populate_overview();
}

/** @brief 删除结果自动返回 Timer；重复调用安全 */
static void cancel_result_timer(void)
{
    if (s_view.result_timer != NULL)
    {
        lv_timer_delete(s_view.result_timer);
        s_view.result_timer = NULL;
    }
}

/** @brief 把菜单标题栏中的直接子标签切换为项目中文字体 */
static void style_menu_header_title(void)
{
    lv_obj_t *header = lv_menu_get_main_header(s_view.menu);
    if (header == NULL)
    {
        return;
    }
    const uint32_t child_count = lv_obj_get_child_count(header);
    for (uint32_t index = 0; index < child_count; ++index)
    {
        lv_obj_t *child = lv_obj_get_child(header, (int32_t) index);
        if (lv_obj_check_type(child, &lv_label_class))
        {
            lv_obj_set_style_text_font(child, ui_platform_font_get_semibold(16), 0);
        }
    }
}

/** @brief 为子页面创建无边框、不可滚动的固定内容容器 */
static lv_obj_t *create_page_body(lv_obj_t *page)
{
    lv_obj_t *body = lv_obj_create(page);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), 212);
    lv_obj_set_style_bg_color(body, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(body, lv_color_black(), 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    return body;
}

/** @brief 根菜单焦点变化时同步反色其文本 */
static void on_root_item_focus(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_user_data(event);
    if (label == NULL)
    {
        return;
    }
    const bool focused = lv_event_get_code(event) == LV_EVENT_FOCUSED;
    lv_obj_set_style_text_color(label, focused ? lv_color_white() : lv_color_black(), 0);
}

/** @brief 清空子页焦点成员，随后由当前状态按需注册唯一操作对象 */
static void prepare_child_group(void)
{
    s_view.child_action = NULL;
    if (s_view.group != NULL)
    {
        lv_group_remove_all_objs(s_view.group);
    }
}

/** @brief 创建子页唯一操作项并使其成为 lv_group 当前焦点 */
static lv_obj_t *create_child_action(lv_obj_t *parent, const char *title, int32_t y, lv_event_cb_t callback)
{
    lv_obj_t *action = lv_obj_create(parent);
    lv_obj_set_pos(action, 12, y);
    lv_obj_set_size(action, 360, 38);
    lv_obj_set_style_bg_color(action, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(action, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(action, lv_color_black(), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(action, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(action, lv_color_black(), 0);
    lv_obj_set_style_border_width(action, 1, 0);
    lv_obj_set_style_radius(action, 0, 0);
    lv_obj_set_style_pad_all(action, 0, 0);
    lv_obj_clear_flag(action, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(action, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = ui_common_new_text16_semibold(action);
    lv_label_set_text(label, title);
    lv_obj_center(label);
    lv_obj_add_event_cb(action, on_root_item_focus, LV_EVENT_FOCUSED, label);
    lv_obj_add_event_cb(action, on_root_item_focus, LV_EVENT_DEFOCUSED, label);
    lv_obj_add_event_cb(action, callback, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(s_view.group, action);
    lv_group_focus_obj(action);
    s_view.child_action = action;
    return action;
}

/** @brief 创建根菜单项并将其绑定到唯一子页面 */
static lv_obj_t *create_root_item(settings_item_t item, lv_obj_t *target_page, const char *title)
{
    lv_obj_t *container = lv_menu_cont_create(s_view.root_page);
    lv_obj_set_size(container, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(container, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(container, lv_color_black(), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_left(container, 18, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = ui_common_new_text16_semibold(container);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_menu_set_load_page_event(s_view.menu, container, target_page);
    lv_obj_add_event_cb(container, on_root_item_focus, LV_EVENT_FOCUSED, label);
    lv_obj_add_event_cb(container, on_root_item_focus, LV_EVENT_DEFOCUSED, label);
    lv_group_add_obj(s_view.group, container);
    s_view.root_items[item] = container;
    return container;
}

/** @brief 绘制网络详情或配网 Portal 信息 */
static void render_network(bool action_attempted, esp_err_t action_error)
{
    settings_view_model_t view;
    settings_presenter_get_view_copy(&view);
    prepare_child_group();
    lv_obj_clean(s_view.network_body);

    if (view.portal.active)
    {
        (void) new_text24(s_view.network_body, "配网中", 12, 4, 220, 30);

        char text[128];
        (void) snprintf(text, sizeof(text), "热点  %s", view.portal.ssid[0] != '\0' ? view.portal.ssid : "--");
        (void) new_text16(s_view.network_body, text, 12, 44, 236, 22);
        (void) snprintf(text, sizeof(text), "密码  %s", view.portal.password[0] != '\0' ? view.portal.password : "无");
        (void) new_text16(s_view.network_body, text, 12, 74, 236, 22);
        (void)
            snprintf(text, sizeof(text), "地址  %s", view.portal.portal_url[0] != '\0' ? view.portal.portal_url : "--");
        (void) new_text16(s_view.network_body, text, 12, 104, 236, 44);
        (void) new_text16(s_view.network_body, "左长按返回，配网服务保持运行", 12, 176, 360, 22);
#if LV_USE_QRCODE
        if (view.portal.wifi_qr[0] != '\0')
        {
            lv_obj_t *qr = lv_qrcode_create(s_view.network_body);
            if (qr != NULL)
            {
                lv_qrcode_set_size(qr, 112);
                lv_qrcode_set_dark_color(qr, lv_color_black());
                lv_qrcode_set_light_color(qr, lv_color_white());
                lv_qrcode_set_quiet_zone(qr, true);
                lv_qrcode_set_data(qr, view.portal.wifi_qr);
                lv_obj_set_pos(qr, 264, 38);
            }
        }
#endif
        return;
    }

    (void) new_text24(s_view.network_body, network_state_text(&view), 12, 4, 360, 30);

    char text[96];
    (void) snprintf(text, sizeof(text), "SSID  %s", view.station_ssid[0] != '\0' ? view.station_ssid : "--");
    (void) new_text16(s_view.network_body, text, 12, 48, 360, 22);

    (void) snprintf(text, sizeof(text), "IP    %s", view.station_ip[0] != '\0' ? view.station_ip : "--");
    (void) new_text16(s_view.network_body, text, 12, 78, 360, 22);

    char rssi[32] = "--";
    if (view.wifi_connected)
    {
        ui_format_rssi(view.rssi_dbm, rssi, sizeof(rssi));
    }
    (void) snprintf(text, sizeof(text), "RSSI  %s", rssi);
    (void) new_text16(s_view.network_body, text, 12, 108, 360, 22);

    if (action_attempted && action_error == ESP_OK)
    {
        (void) new_text16(s_view.network_body, "正在启动配网…", 12, 160, 360, 22);
    }
    else if (action_error != ESP_OK)
    {
        (void) snprintf(text, sizeof(text), "启动失败：%s，请重试", esp_err_to_name(action_error));
        (void) new_text16(s_view.network_body, text, 12, 142, 360, 22);
        (void) create_child_action(s_view.network_body, "长按右键重试配网", 170, on_network_action_clicked);
    }
    else
    {
        (void) create_child_action(s_view.network_body,
                                   view.network_state == SETTINGS_NETWORK_VIEW_FAILED ? "长按右键重试配网"
                                                                                      : "长按右键启动配网",
                                   160,
                                   on_network_action_clicked);
    }
}

/** @brief 网络子页操作项点击后上报启动 Portal 意图并原地显示结果 */
static void on_network_action_clicked(lv_event_t *event)
{
    (void) event;
    const ui_user_intent_t intent = {
        .id = UI_USER_INTENT_SETTINGS_START_PORTAL,
    };
    const esp_err_t error = ui_runtime_emit_user_intent(&intent);
    render_network(true, error);
}

/**
 * @brief 按网页文件 Presenter 快照完整重绘子页，并只在安全终态完成待定返回
 *
 * `web_file_exit_pending` 只表示停止请求已被 Application 接收；页面仍等待 Presenter 报告
 * `STOPPED` 或允许退出的错误态。清理错误保留在当前子页，等待用户再次长按左键请求停止。
 *
 * @param[in] action_error 最近一次 UI 意图同步提交结果
 */
static void render_web_file(esp_err_t action_error)
{
    web_file_view_model_t view;
    web_file_presenter_get_view_copy(&view);

    if (s_view.web_file_exit_pending
        && (view.state == WEB_FILE_PRESENTER_STATE_STOPPED
            || (view.state == WEB_FILE_PRESENTER_STATE_ERROR && view.exit_allowed)))
    {
        s_view.web_file_exit_pending = false;
        return_to_root();
        return;
    }

    if (s_view.web_file_exit_pending && view.state == WEB_FILE_PRESENTER_STATE_ERROR && !view.exit_allowed)
    {
        s_view.web_file_exit_pending = false;
    }

    prepare_child_group();
    lv_obj_clean(s_view.web_file_body);

    char            text[128];
    const esp_err_t display_error = view.error != ESP_OK ? view.error : action_error;
    const bool      start_failed  = (view.state == WEB_FILE_PRESENTER_STATE_ERROR && view.exit_allowed)
                                    || (view.state == WEB_FILE_PRESENTER_STATE_STOPPED && action_error != ESP_OK);

    if (s_view.web_file_exit_pending)
    {
        (void) new_text24(s_view.web_file_body, "正在关闭服务", 12, 28, 360, 30);
        (void) new_text16(s_view.web_file_body, "请等待文件传输与网络资源安全释放", 12, 78, 360, 22);
        return;
    }

    if (view.state == WEB_FILE_PRESENTER_STATE_RUNNING)
    {
        (void) new_text24(s_view.web_file_body, view.title, 12, 4, 360, 30);
        (void) snprintf(text, sizeof(text), "地址  %s", view.url[0] != '\0' ? view.url : "--");
        (void) new_text16(s_view.web_file_body, text, 12, 42, 360, 22);
        (void) snprintf(text, sizeof(text), "访问码  %s", view.access_code[0] != '\0' ? view.access_code : "------");
        (void) new_text16(s_view.web_file_body, text, 12, 72, 360, 22);
        (void) snprintf(text, sizeof(text), "总容量  %s", view.total_size);
        (void) new_text16(s_view.web_file_body, text, 12, 102, 360, 22);
        (void) snprintf(text, sizeof(text), "剩余容量  %s", view.free_size);
        (void) new_text16(s_view.web_file_body, text, 12, 132, 360, 22);
        if (action_error != ESP_OK)
        {
            (void) snprintf(text, sizeof(text), "操作失败：%s", esp_err_to_name(action_error));
            (void) new_text16(s_view.web_file_body, text, 12, 162, 360, 22);
        }
        (void) new_text16(s_view.web_file_body, "长按左键关闭并返回", 12, 190, 360, 18);
        return;
    }

    if (start_failed)
    {
        (void) new_text24(s_view.web_file_body, "启动失败", 12, 24, 360, 30);
        (void) snprintf(text, sizeof(text), "错误：%s", esp_err_to_name(display_error));
        (void) new_text16(s_view.web_file_body, text, 12, 70, 360, 22);
        (void) create_child_action(s_view.web_file_body, "长按右键重试", 132, on_web_file_retry_clicked);
        (void) new_text16(s_view.web_file_body, "长按左键返回", 12, 188, 360, 18);
        return;
    }

    if (view.state == WEB_FILE_PRESENTER_STATE_ERROR)
    {
        (void) new_text24(s_view.web_file_body, "关闭失败", 12, 24, 360, 30);
        (void) snprintf(text, sizeof(text), "错误：%s", esp_err_to_name(display_error));
        (void) new_text16(s_view.web_file_body, text, 12, 70, 360, 22);
        (void) new_text16(s_view.web_file_body, "仍在保留服务或网络资源", 12, 108, 360, 22);
        (void) new_text16(s_view.web_file_body, "长按左键重试关闭", 12, 166, 360, 22);
        return;
    }

    (void) new_text24(s_view.web_file_body, view.title, 12, 34, 360, 30);
    if (view.state == WEB_FILE_PRESENTER_STATE_STOPPED)
    {
        (void) new_text16(s_view.web_file_body, "服务未启动", 12, 82, 360, 22);
        (void) new_text16(s_view.web_file_body, "长按左键返回", 12, 166, 360, 22);
    }
    else if (view.state == WEB_FILE_PRESENTER_STATE_STOPPING)
    {
        (void) new_text16(s_view.web_file_body, "请等待资源安全释放", 12, 82, 360, 22);
    }
    else
    {
        (void) new_text16(s_view.web_file_body, "请稍候", 12, 82, 360, 22);
        (void) new_text16(s_view.web_file_body, "长按左键可请求停止", 12, 166, 360, 22);
    }

    if (action_error != ESP_OK)
    {
        (void) snprintf(text, sizeof(text), "操作失败：%s", esp_err_to_name(action_error));
        (void) new_text16(s_view.web_file_body, text, 12, 120, 360, 22);
    }
}

/** @brief 网页文件启动失败操作项点击后重新提交启动意图 */
static void on_web_file_retry_clicked(lv_event_t *event)
{
    (void) event;
    s_view.web_file_exit_pending  = false;
    const ui_user_intent_t intent = {
        .id = UI_USER_INTENT_SETTINGS_START_WEB_FILE,
    };
    const esp_err_t error = ui_runtime_emit_user_intent(&intent);
    render_web_file(error);
}

/** @brief 将固件字节数格式化为紧凑 KiB/MiB 文本 */
static void format_firmware_size(size_t bytes, char *output, size_t output_size)
{
    const size_t mib = 1024U * 1024U;
    if (bytes >= mib)
    {
        const size_t tenths = (bytes % mib) * 10U / mib;
        (void) snprintf(output, output_size, "%lu.%lu MiB", (unsigned long) (bytes / mib), (unsigned long) tenths);
    }
    else
    {
        (void) snprintf(output, output_size, "%lu KiB", (unsigned long) ((bytes + 1023U) / 1024U));
    }
}

/** @brief 结果页自动返回回调；Timer 触发一次后由 LVGL 自行释放 */
static void on_result_auto_return(lv_timer_t *timer)
{
    (void) timer;
    s_view.result_timer = NULL;
    if (s_view.location == SETTINGS_LOCATION_OTA)
    {
        return_to_root();
    }
}

/** @brief 在无更新或检查失败首次出现时启动一次性返回 Timer */
static void sync_result_timer(ota_presenter_state_t state)
{
    if ((int) state == s_view.rendered_ota_state)
    {
        return;
    }

    cancel_result_timer();
    s_view.rendered_ota_state = (int) state;
    if (state == OTA_PRESENTER_STATE_NO_UPDATE || state == OTA_PRESENTER_STATE_CHECK_FAILED)
    {
        s_view.result_timer =
            lv_timer_create(on_result_auto_return, CONFIG_DESKMATE_UI_SETTINGS_RESULT_AUTO_RETURN_MS, NULL);
        if (s_view.result_timer != NULL)
        {
            lv_timer_set_repeat_count(s_view.result_timer, 1);
        }
    }
}

/** @brief 按 OTA Presenter 快照完整重绘更新子页 */
static void render_ota(esp_err_t action_error)
{
    ota_presenter_view_model_t view;
    if (ota_presenter_get_view_copy(&view) != ESP_OK)
    {
        return;
    }
    sync_result_timer(view.state);
    prepare_child_group();
    lv_obj_clean(s_view.ota_body);

    char text[128];
    (void) snprintf(text, sizeof(text), "当前版本  %s", view.current_version[0] != '\0' ? view.current_version : "--");
    (void) new_text16(s_view.ota_body, text, 12, 8, 360, 22);

    switch (view.state)
    {
        case OTA_PRESENTER_STATE_CHECKING:
            (void) new_text24(s_view.ota_body, "正在检查更新", 12, 52, 360, 30);
            (void) new_text16(s_view.ota_body, "检查完成前暂时无法返回", 12, 100, 360, 22);
            break;
        case OTA_PRESENTER_STATE_NO_UPDATE:
            (void) new_text24(s_view.ota_body, "已是最新版本", 12, 52, 360, 30);
            (void) new_text16(s_view.ota_body, "3 秒后自动返回", 12, 100, 360, 22);
            break;
        case OTA_PRESENTER_STATE_AVAILABLE: {
            (void) new_text24(s_view.ota_body, "发现可用更新", 12, 36, 360, 30);
            (void) snprintf(text,
                            sizeof(text),
                            "目标版本  %s",
                            view.target_version[0] != '\0' ? view.target_version : "--");
            (void) new_text16(s_view.ota_body, text, 12, 72, 360, 22);
            char size_text[32];
            format_firmware_size(view.target_size_bytes, size_text, sizeof(size_text));
            (void) snprintf(text, sizeof(text), "固件大小  %s", size_text);
            (void) new_text16(s_view.ota_body, text, 12, 100, 360, 22);
            const esp_err_t error = view.error != ESP_OK ? view.error : action_error;
            if (error != ESP_OK)
            {
                (void) snprintf(text, sizeof(text), "操作失败：%s，请重试", esp_err_to_name(error));
                (void) new_text16(s_view.ota_body, text, 12, 126, 360, 22);
            }
            (void) create_child_action(s_view.ota_body, "长按右键安装", 150, on_ota_install_clicked);
            (void) new_text16(s_view.ota_body, "长按左键取消", 12, 192, 360, 18);
            break;
        }
        case OTA_PRESENTER_STATE_DOWNLOADING:
            (void) new_text24(s_view.ota_body, "正在下载并安装", 12, 52, 360, 30);
            (void) new_text16(s_view.ota_body, "请勿断电", 12, 100, 360, 22);
            (void) new_text16(s_view.ota_body, "安装完成后设备将自动重启", 12, 132, 360, 22);
            break;
        case OTA_PRESENTER_STATE_CHECK_FAILED:
            (void) new_text24(s_view.ota_body, "检查更新失败", 12, 48, 360, 30);
            (void) snprintf(text,
                            sizeof(text),
                            "错误：%s",
                            esp_err_to_name(view.error != ESP_OK ? view.error : action_error));
            (void) new_text16(s_view.ota_body, text, 12, 92, 360, 22);
            (void) new_text16(s_view.ota_body, "3 秒后自动返回", 12, 126, 360, 22);
            break;
        case OTA_PRESENTER_STATE_INSTALL_FAILED:
            (void) new_text24(s_view.ota_body, "安装失败", 12, 48, 360, 30);
            (void) snprintf(text,
                            sizeof(text),
                            "错误：%s",
                            esp_err_to_name(view.error != ESP_OK ? view.error : action_error));
            (void) new_text16(s_view.ota_body, text, 12, 92, 360, 22);
            (void) new_text16(s_view.ota_body, "长按左键返回", 12, 138, 360, 22);
            break;
        case OTA_PRESENTER_STATE_IDLE:
        default:
            (void) new_text24(s_view.ota_body, "准备检查更新", 12, 52, 360, 30);
            if (action_error != ESP_OK)
            {
                (void) snprintf(text, sizeof(text), "请求失败：%s", esp_err_to_name(action_error));
                (void) new_text16(s_view.ota_body, text, 12, 96, 360, 22);
            }
            break;
    }
}

/** @brief OTA 安装操作项点击后提交安装意图并按最新快照原地重绘 */
static void on_ota_install_clicked(lv_event_t *event)
{
    (void) event;
    const ui_user_intent_t intent = {
        .id = UI_USER_INTENT_SETTINGS_OTA_INSTALL,
    };
    const esp_err_t error = ui_runtime_emit_user_intent(&intent);
    render_ota(error);
}

/** @brief 返回番茄钟配置项的稳定中文名称 */
static const char *pomodoro_setting_title(uint8_t index)
{
    static const char *const titles[] = {
        "专注时长",
        "短休时长",
        "长休时长",
        "长休间隔",
    };
    return index < 4U ? titles[index] : "";
}

/** @brief 返回草稿中指定配置项的当前值 */
static uint8_t pomodoro_draft_value(uint8_t index)
{
    switch (index)
    {
        case 0:
            return s_view.pomodoro_draft.focus_minutes;
        case 1:
            return s_view.pomodoro_draft.short_break_minutes;
        case 2:
            return s_view.pomodoro_draft.long_break_minutes;
        case 3:
        default:
            return s_view.pomodoro_draft.long_break_interval;
    }
}

/** @brief 创建一行番茄钟配置并按当前选择状态反白 */
static void create_pomodoro_row(uint8_t index)
{
    lv_obj_t *row = lv_obj_create(s_view.pomodoro_body);
    lv_obj_set_pos(row, 12, 4 + (int32_t) index * 40);
    lv_obj_set_size(row, 360, 36);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    const bool selected = index == s_view.pomodoro_selected;
    lv_obj_set_style_bg_color(row, selected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_common_new_text16_semibold(row);
    ui_common_set_label(title, pomodoro_setting_title(index), 10, 8, 210, 20, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *value = ui_common_new_text16_regular(row);
    char      text[24];
    (void) snprintf(text,
                    sizeof(text),
                    index == 3U ? "%u 轮" : "%u 分钟",
                    (unsigned) pomodoro_draft_value(index));
    ui_common_set_label(value, text, 220, 8, 128, 20, LV_TEXT_ALIGN_RIGHT);
    if (selected)
    {
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_color(value, lv_color_white(), 0);
    }
}

/** @brief 按 Presenter 快照重绘番茄钟设置列表或单项编辑状态 */
static void render_pomodoro(esp_err_t action_error)
{
    pomodoro_view_model_t view;
    if (pomodoro_presenter_get_view_copy(&view) != ESP_OK)
    {
        return;
    }
    prepare_child_group();
    lv_obj_clean(s_view.pomodoro_body);
    if (!s_view.pomodoro_editing)
    {
        s_view.pomodoro_draft = (ui_pomodoro_settings_intent_t) {
            .focus_minutes       = view.focus_minutes,
            .short_break_minutes = view.short_break_minutes,
            .long_break_minutes  = view.long_break_minutes,
            .long_break_interval = view.long_break_interval,
        };
    }
    for (uint8_t index = 0U; index < 4U; ++index)
    {
        create_pomodoro_row(index);
    }

    const bool editable = view.run_state == POMODORO_VIEW_RUN_IDLE;
    const char *hint = s_view.pomodoro_editing
                           ? "左右调整 / 右长保存 / 左长取消"
                           : (editable ? "左右选择 / 右长编辑 / 左长返回"
                                       : "结束当前番茄钟后可修改");
    (void) new_text16(s_view.pomodoro_body, hint, 12, 168, 360, 20);
    if (!view.settings_saved)
    {
        (void) new_text16(s_view.pomodoro_body, "未保存，将继续使用当前内存值", 12, 190, 360, 20);
    }
    else if (action_error != ESP_OK)
    {
        char error_text[64];
        (void) snprintf(error_text, sizeof(error_text), "保存请求失败：%s", esp_err_to_name(action_error));
        (void) new_text16(s_view.pomodoro_body, error_text, 12, 190, 360, 20);
    }
}

/** @brief 按范围和步长调整当前番茄钟配置草稿 */
static void adjust_pomodoro_draft(bool increase)
{
    uint8_t *value = NULL;
    uint8_t  minimum = 0U;
    uint8_t  maximum = 0U;
    uint8_t  step    = 1U;
    switch (s_view.pomodoro_selected)
    {
        case 0:
            value   = &s_view.pomodoro_draft.focus_minutes;
            minimum = 5U;
            maximum = 90U;
            step    = 5U;
            break;
        case 1:
            value   = &s_view.pomodoro_draft.short_break_minutes;
            minimum = 1U;
            maximum = 30U;
            break;
        case 2:
            value   = &s_view.pomodoro_draft.long_break_minutes;
            minimum = 5U;
            maximum = 60U;
            step    = 5U;
            break;
        case 3:
        default:
            value   = &s_view.pomodoro_draft.long_break_interval;
            minimum = 2U;
            maximum = 8U;
            break;
    }
    if (increase)
    {
        *value = *value <= (uint8_t) (maximum - step) ? (uint8_t) (*value + step) : maximum;
    }
    else
    {
        *value = *value >= (uint8_t) (minimum + step) ? (uint8_t) (*value - step) : minimum;
    }
}

/** @brief 解释番茄钟设置子页的列表选择、编辑、保存和取消动作 */
static esp_err_t handle_pomodoro_action(presentation_settings_action_t action)
{
    pomodoro_view_model_t view;
    if (pomodoro_presenter_get_view_copy(&view) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (action == PRESENTATION_SETTINGS_ACTION_BACK)
    {
        if (s_view.pomodoro_editing)
        {
            s_view.pomodoro_editing = false;
            render_pomodoro(ESP_OK);
        }
        else
        {
            return_to_root();
        }
        return ESP_OK;
    }
    if (action == PRESENTATION_SETTINGS_ACTION_PREV || action == PRESENTATION_SETTINGS_ACTION_NEXT)
    {
        if (s_view.pomodoro_editing)
        {
            adjust_pomodoro_draft(action == PRESENTATION_SETTINGS_ACTION_NEXT);
        }
        else if (action == PRESENTATION_SETTINGS_ACTION_PREV)
        {
            s_view.pomodoro_selected = s_view.pomodoro_selected == 0U ? 3U : s_view.pomodoro_selected - 1U;
        }
        else
        {
            s_view.pomodoro_selected = (uint8_t) ((s_view.pomodoro_selected + 1U) % 4U);
        }
        render_pomodoro(ESP_OK);
        return ESP_OK;
    }
    if (action != PRESENTATION_SETTINGS_ACTION_ACTIVATE)
    {
        return ESP_OK;
    }
    if (!s_view.pomodoro_editing)
    {
        if (view.run_state == POMODORO_VIEW_RUN_IDLE)
        {
            s_view.pomodoro_editing = true;
            render_pomodoro(ESP_OK);
        }
        return ESP_OK;
    }

    const ui_user_intent_t intent = {
        .id                = UI_USER_INTENT_POMODORO_SETTINGS_SAVE,
        .pomodoro_settings = s_view.pomodoro_draft,
    };
    const esp_err_t error = ui_runtime_emit_user_intent(&intent);
    if (error == ESP_OK)
    {
        s_view.pomodoro_editing = false;
    }
    render_pomodoro(error);
    return error;
}

/** @brief 根菜单项激活后的唯一层级切换与业务意图桥接 */
static void on_root_item_clicked(lv_event_t *event)
{
    const settings_item_t item = (settings_item_t) (uintptr_t) lv_event_get_user_data(event);
    s_view.return_focus        = lv_event_get_current_target(event);
    style_menu_header_title();

    switch (item)
    {
        case SETTINGS_ITEM_NETWORK:
            s_view.location = SETTINGS_LOCATION_NETWORK;
            render_network(false, ESP_OK);
            break;
        case SETTINGS_ITEM_WEB_FILE: {
            s_view.location               = SETTINGS_LOCATION_WEB_FILE;
            s_view.web_file_exit_pending  = false;
            const ui_user_intent_t intent = {
                .id = UI_USER_INTENT_SETTINGS_START_WEB_FILE,
            };
            const esp_err_t error = ui_runtime_emit_user_intent(&intent);
            render_web_file(error);
            break;
        }
        case SETTINGS_ITEM_SYSTEM:
            s_view.location = SETTINGS_LOCATION_SYSTEM;
            prepare_child_group();
            (void) ui_system_page_update(s_view.system_body);
            break;
        case SETTINGS_ITEM_OTA: {
            s_view.location               = SETTINGS_LOCATION_OTA;
            s_view.rendered_ota_state     = -1;
            const ui_user_intent_t intent = {
                .id = UI_USER_INTENT_SETTINGS_OTA_CHECK,
            };
            const esp_err_t error = ui_runtime_emit_user_intent(&intent);
            render_ota(error);
            break;
        }
        case SETTINGS_ITEM_POMODORO:
            s_view.location           = SETTINGS_LOCATION_POMODORO;
            s_view.pomodoro_selected  = 0U;
            s_view.pomodoro_editing   = false;
            render_pomodoro(ESP_OK);
            break;
        default:
            break;
    }
}

/** @brief 为已创建的根菜单项补充项目激活回调 */
static void bind_root_item_callbacks(void)
{
    for (settings_item_t item = SETTINGS_ITEM_NETWORK; item < SETTINGS_ITEM_COUNT; item = (settings_item_t) (item + 1))
    {
        lv_obj_add_event_cb(s_view.root_items[item], on_root_item_clicked, LV_EVENT_CLICKED, (void *) (uintptr_t) item);
    }
}

/** @brief 创建两层菜单、五个子页面和非循环焦点组 */
static esp_err_t open_menu(void)
{
    if (s_view.menu != NULL)
    {
        return ESP_OK;
    }

    lv_obj_clean(s_view.parent);
    s_view.overview_network = NULL;
    s_view.overview_version = NULL;

    s_view.menu             = lv_menu_create(s_view.parent);
    if (s_view.menu == NULL)
    {
        show_overview();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_view.menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_view.menu, 0, 0);
    lv_obj_set_style_bg_color(s_view.menu, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_view.menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_view.menu, 0, 0);
    lv_menu_set_mode_header(s_view.menu, LV_MENU_HEADER_TOP_FIXED);
    lv_menu_set_mode_root_back_button(s_view.menu, LV_MENU_ROOT_BACK_BUTTON_DISABLED);

    s_view.group = lv_group_create();
    if (s_view.group == NULL)
    {
        lv_obj_delete(s_view.menu);
        s_view.menu = NULL;
        show_overview();
        return ESP_ERR_NO_MEM;
    }
    lv_group_set_wrap(s_view.group, true);

    s_view.root_page     = lv_menu_page_create(s_view.menu, NULL);
    s_view.network_page  = lv_menu_page_create(s_view.menu, "网络设置");
    s_view.web_file_page = lv_menu_page_create(s_view.menu, "网页文件管理");
    s_view.system_page   = lv_menu_page_create(s_view.menu, "系统信息");
    s_view.ota_page      = lv_menu_page_create(s_view.menu, "检查更新");
    s_view.pomodoro_page = lv_menu_page_create(s_view.menu, "番茄钟设置");

    lv_obj_set_style_pad_all(s_view.root_page, 0, 0);
    lv_obj_set_style_pad_row(s_view.root_page, 0, 0);
    s_view.network_body  = create_page_body(s_view.network_page);
    s_view.web_file_body = create_page_body(s_view.web_file_page);
    s_view.system_body   = create_page_body(s_view.system_page);
    s_view.ota_body      = create_page_body(s_view.ota_page);
    s_view.pomodoro_body = create_page_body(s_view.pomodoro_page);

    (void) create_root_item(SETTINGS_ITEM_NETWORK, s_view.network_page, "网络设置");
    (void) create_root_item(SETTINGS_ITEM_WEB_FILE, s_view.web_file_page, "网页文件管理");
    (void) create_root_item(SETTINGS_ITEM_SYSTEM, s_view.system_page, "系统信息");
    (void) create_root_item(SETTINGS_ITEM_OTA, s_view.ota_page, "检查更新");
    (void) create_root_item(SETTINGS_ITEM_POMODORO, s_view.pomodoro_page, "番茄钟设置");
    bind_root_item_callbacks();

    lv_menu_set_page(s_view.menu, s_view.root_page);
    style_menu_header_title();
    lv_group_focus_obj(s_view.root_items[SETTINGS_ITEM_NETWORK]);
    s_view.return_focus       = s_view.root_items[SETTINGS_ITEM_NETWORK];
    s_view.location           = SETTINGS_LOCATION_ROOT;
    s_view.rendered_ota_state = -1;
    return ESP_OK;
}

/** @brief 从子页退回根页并恢复进入前的菜单焦点 */
static void return_to_root(void)
{
    cancel_result_timer();
    if (s_view.menu == NULL)
    {
        return;
    }

    prepare_child_group();
    lv_obj_t *back_button = lv_menu_get_main_header_back_button(s_view.menu);
    if (back_button != NULL)
    {
        (void) lv_obj_send_event(back_button, LV_EVENT_CLICKED, NULL);
    }
    else
    {
        lv_menu_set_page(s_view.menu, s_view.root_page);
    }
    style_menu_header_title();
    s_view.location              = SETTINGS_LOCATION_ROOT;
    s_view.rendered_ota_state    = -1;
    s_view.web_file_exit_pending = false;
    for (settings_item_t item = SETTINGS_ITEM_NETWORK; item < SETTINGS_ITEM_COUNT; item = (settings_item_t) (item + 1))
    {
        lv_group_add_obj(s_view.group, s_view.root_items[item]);
    }
    if (s_view.return_focus != NULL)
    {
        lv_group_focus_obj(s_view.return_focus);
    }
}

/** @brief 请求 Application 关闭设置会话，成功后销毁菜单并恢复概览 */
static esp_err_t close_menu(void)
{
    const ui_user_intent_t intent = {
        .id = UI_USER_INTENT_SETTINGS_MENU_CLOSED,
    };
    const esp_err_t error = ui_runtime_emit_user_intent(&intent);
    if (error != ESP_OK)
    {
        return error;
    }

    cancel_result_timer();
    if (s_view.group != NULL)
    {
        lv_group_delete(s_view.group);
        s_view.group = NULL;
    }
    if (s_view.menu != NULL)
    {
        lv_obj_delete(s_view.menu);
    }

    lv_obj_t *parent = s_view.parent;
    memset(&s_view, 0, sizeof(s_view));
    s_view.parent             = parent;
    s_view.location           = SETTINGS_LOCATION_OVERVIEW;
    s_view.rendered_ota_state = -1;
    show_overview();
    return ESP_OK;
}

/** @brief 处理 OTA 子页动作并执行状态相关的导航锁定规则 */
static esp_err_t handle_ota_action(presentation_settings_action_t action)
{
    ota_presenter_view_model_t view;
    const esp_err_t            read_error = ota_presenter_get_view_copy(&view);
    if (read_error != ESP_OK)
    {
        return read_error;
    }

    if (view.state == OTA_PRESENTER_STATE_CHECKING || view.state == OTA_PRESENTER_STATE_DOWNLOADING)
    {
        return ESP_OK;
    }

    if (action == PRESENTATION_SETTINGS_ACTION_BACK)
    {
        if (view.state == OTA_PRESENTER_STATE_AVAILABLE)
        {
            const ui_user_intent_t intent = {
                .id = UI_USER_INTENT_SETTINGS_OTA_DISCARD,
            };
            const esp_err_t error = ui_runtime_emit_user_intent(&intent);
            render_ota(error);
            if (error != ESP_OK)
            {
                return error;
            }
        }
        return_to_root();
        return ESP_OK;
    }

    if (action == PRESENTATION_SETTINGS_ACTION_ACTIVATE && view.state == OTA_PRESENTER_STATE_AVAILABLE
        && s_view.child_action != NULL)
    {
        (void) lv_obj_send_event(s_view.child_action, LV_EVENT_CLICKED, NULL);
    }
    return ESP_OK;
}

/**
 * @brief 处理网页文件子页动作，并在 Application 安全收敛前锁定导航
 *
 * Back 只提交非阻塞停止意图；真正返回由 `render_web_file()` 在 Presenter 报告安全终态后
 * 完成。启动或停止进行中的焦点与激活动作只被消费，不改变页面层级。
 *
 * @param[in] action 当前设置菜单动作
 * @return ESP_OK 动作已消费或意图已提交；其他值表示状态读取或意图提交失败
 */
static esp_err_t handle_web_file_action(presentation_settings_action_t action)
{
    web_file_view_model_t view;
    web_file_presenter_get_view_copy(&view);

    if (action == PRESENTATION_SETTINGS_ACTION_BACK && view.state == WEB_FILE_PRESENTER_STATE_STOPPED)
    {
        s_view.web_file_exit_pending = false;
        return_to_root();
        return ESP_OK;
    }

    if (action == PRESENTATION_SETTINGS_ACTION_BACK)
    {
        const ui_user_intent_t intent = {
            .id = UI_USER_INTENT_SETTINGS_STOP_WEB_FILE,
        };
        const esp_err_t error        = ui_runtime_emit_user_intent(&intent);
        s_view.web_file_exit_pending = error == ESP_OK;
        render_web_file(error);
        return error;
    }

    if (action == PRESENTATION_SETTINGS_ACTION_ACTIVATE
        && ((view.state == WEB_FILE_PRESENTER_STATE_ERROR && view.exit_allowed)
            || (view.state == WEB_FILE_PRESENTER_STATE_STOPPED && s_view.child_action != NULL)))
    {
        if (s_view.child_action != NULL)
        {
            (void) lv_obj_send_event(s_view.child_action, LV_EVENT_CLICKED, NULL);
        }
        else
        {
            const ui_user_intent_t intent = {
                .id = UI_USER_INTENT_SETTINGS_START_WEB_FILE,
            };
            const esp_err_t error = ui_runtime_emit_user_intent(&intent);
            render_web_file(error);
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t ui_settings_page_init(void)
{
    return ui_system_page_init();
}

void ui_settings_page_deinit(void)
{
    cancel_result_timer();
    if (s_view.group != NULL)
    {
        lv_group_delete(s_view.group);
    }
    memset(&s_view, 0, sizeof(s_view));
    s_view.location           = SETTINGS_LOCATION_OVERVIEW;
    s_view.rendered_ota_state = -1;
}

esp_err_t ui_settings_page_show(lv_obj_t *parent)
{
    if (parent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ui_settings_page_deinit();
    s_view.parent = parent;
    show_overview();
    return ESP_OK;
}

esp_err_t ui_settings_page_update(lv_obj_t *parent)
{
    if (parent == NULL || parent != s_view.parent)
    {
        return ESP_ERR_INVALID_ARG;
    }

    switch (s_view.location)
    {
        case SETTINGS_LOCATION_OVERVIEW:
            populate_overview();
            break;
        case SETTINGS_LOCATION_NETWORK:
            render_network(false, ESP_OK);
            break;
        case SETTINGS_LOCATION_WEB_FILE:
            render_web_file(ESP_OK);
            break;
        case SETTINGS_LOCATION_SYSTEM:
            return ui_system_page_update(s_view.system_body);
        case SETTINGS_LOCATION_OTA:
            render_ota(ESP_OK);
            break;
        case SETTINGS_LOCATION_POMODORO:
            render_pomodoro(ESP_OK);
            break;
        case SETTINGS_LOCATION_ROOT:
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t ui_settings_page_handle_action(presentation_settings_action_t action)
{
    if ((unsigned) action > PRESENTATION_SETTINGS_ACTION_BACK)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_view.location == SETTINGS_LOCATION_OVERVIEW)
    {
        if (action != PRESENTATION_SETTINGS_ACTION_OPEN)
        {
            return ESP_OK;
        }
        const esp_err_t error = open_menu();
        if (error != ESP_OK)
        {
            const ui_user_intent_t intent = {
                .id = UI_USER_INTENT_SETTINGS_MENU_CLOSED,
            };
            (void) ui_runtime_emit_user_intent(&intent);
        }
        return error;
    }
    if (s_view.menu == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    switch (s_view.location)
    {
        case SETTINGS_LOCATION_ROOT:
            switch (action)
            {
                case PRESENTATION_SETTINGS_ACTION_PREV:
                    lv_group_focus_prev(s_view.group);
                    return ESP_OK;
                case PRESENTATION_SETTINGS_ACTION_NEXT:
                    lv_group_focus_next(s_view.group);
                    return ESP_OK;
                case PRESENTATION_SETTINGS_ACTION_ACTIVATE: {
                    lv_obj_t *focused = lv_group_get_focused(s_view.group);
                    if (focused != NULL)
                    {
                        (void) lv_obj_send_event(focused, LV_EVENT_CLICKED, NULL);
                    }
                    return ESP_OK;
                }
                case PRESENTATION_SETTINGS_ACTION_BACK:
                    return close_menu();
                case PRESENTATION_SETTINGS_ACTION_OPEN:
                default:
                    return ESP_OK;
            }
        case SETTINGS_LOCATION_NETWORK:
            if (action == PRESENTATION_SETTINGS_ACTION_BACK)
            {
                return_to_root();
            }
            else if (action == PRESENTATION_SETTINGS_ACTION_ACTIVATE)
            {
                if (s_view.child_action != NULL)
                {
                    (void) lv_obj_send_event(s_view.child_action, LV_EVENT_CLICKED, NULL);
                }
            }
            else if (action == PRESENTATION_SETTINGS_ACTION_PREV)
            {
                lv_group_focus_prev(s_view.group);
            }
            else if (action == PRESENTATION_SETTINGS_ACTION_NEXT)
            {
                lv_group_focus_next(s_view.group);
            }
            return ESP_OK;
        case SETTINGS_LOCATION_WEB_FILE:
            return handle_web_file_action(action);
        case SETTINGS_LOCATION_SYSTEM:
            if (action == PRESENTATION_SETTINGS_ACTION_BACK)
            {
                return_to_root();
            }
            else if (action == PRESENTATION_SETTINGS_ACTION_PREV)
            {
                lv_group_focus_prev(s_view.group);
            }
            else if (action == PRESENTATION_SETTINGS_ACTION_NEXT)
            {
                lv_group_focus_next(s_view.group);
            }
            return ESP_OK;
        case SETTINGS_LOCATION_OTA:
            if (action == PRESENTATION_SETTINGS_ACTION_PREV)
            {
                lv_group_focus_prev(s_view.group);
                return ESP_OK;
            }
            if (action == PRESENTATION_SETTINGS_ACTION_NEXT)
            {
                lv_group_focus_next(s_view.group);
                return ESP_OK;
            }
            return handle_ota_action(action);
        case SETTINGS_LOCATION_POMODORO:
            return handle_pomodoro_action(action);
        case SETTINGS_LOCATION_OVERVIEW:
        default:
            return ESP_OK;
    }
}
