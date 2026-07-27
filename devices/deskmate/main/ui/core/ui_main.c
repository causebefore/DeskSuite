/*
 * 文件职责：同步创建 UI 控件树并解释单条渲染消息。
 * 主要依赖：ui_platform、ui_router、ui_status_bar。
 * 调用方：app_main、ui_task。
 */
#include "ui_main.h"
#include "status_bar_presenter.h"
#include "esp_check.h"
#include "ui_common.h"
#include "ui_platform_lvgl.h"
#include "ui_pomodoro_banner.h"
#include "ui_router.h"
#include "ui_settings_page.h"
#include "ui_status_bar.h"

static lv_obj_t *s_status_bar;
static bool      s_initialized;

/**
 * @brief 拉取最新状态栏快照并同步刷新到状态栏控件
 *
 * @return ESP_OK 刷新成功；其他值由 status_bar_presenter_get_view_copy 或 ui_status_bar_update 返回
 */
static esp_err_t refresh_status_bar(void)
{
    status_bar_view_model_t status_bar;
    esp_err_t               err = status_bar_presenter_get_view_copy(&status_bar);
    if (err != ESP_OK)
    {
        return err;
    }

    return ui_status_bar_update(&status_bar);
}

/**
 * @brief 在顶层图层创建不会随 Screen 切换移动的状态栏容器
 */
static void create_root_layout(void)
{
    s_status_bar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_status_bar);
    lv_obj_set_size(s_status_bar, LV_PCT(100), 32);
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_status_bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);
}

esp_err_t ui_main_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(ui_common_init(), cleanup, "ui_main", "UI 公共样式初始化失败");
    create_root_layout();
    /* 持锁段内任何子模块初始化失败都必须跳到 cleanup 释放锁，避免死锁。
     * ESP_GOTO_ON_ERROR 复用本地变量 ret（见 esp_check.h），故函数内统一用 ret。 */
    ESP_GOTO_ON_ERROR(ui_status_bar_init(), cleanup, "ui_main", "状态栏初始化失败");
    ESP_GOTO_ON_ERROR(ui_pomodoro_banner_init(), cleanup, "ui_main", "番茄钟完成提示初始化失败");
    ESP_GOTO_ON_ERROR(ui_router_init(), cleanup, "ui_main", "UI 路由初始化失败");
    ESP_GOTO_ON_ERROR(ui_platform_lvgl_request_refresh(), cleanup, "ui_main", "请求首帧失败");
    s_initialized = true;
    return ESP_OK;

cleanup:
    (void) ui_main_deinit();
    return ret;
}

/**
 * @brief 取消 UI 定时活动、删除控件树并恢复全部可重入状态
 */
esp_err_t ui_main_deinit(void)
{
    ui_router_deinit();
    ui_pomodoro_banner_deinit();
    ui_status_bar_deinit();
    if (s_status_bar != NULL)
    {
        lv_obj_delete(s_status_bar);
    }
    ui_common_deinit();
    s_status_bar  = NULL;
    s_initialized = false;
    return ESP_OK;
}

esp_err_t ui_main_resync(const ui_msg_t *pending_page_switch)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, "ui_main", "UI 控件树尚未初始化");
    if (pending_page_switch != NULL)
    {
        ESP_RETURN_ON_FALSE(pending_page_switch->type == UI_MSG_SWITCH_PAGE,
                            ESP_ERR_INVALID_ARG,
                            "ui_main",
                            "恢复页面消息类型无效");
        ESP_RETURN_ON_ERROR(ui_router_switch_to(pending_page_switch->page, (ui_nav_dir_t) pending_page_switch->param),
                            "ui_main",
                            "恢复最后页面失败");
    }
    ESP_RETURN_ON_ERROR(refresh_status_bar(), "ui_main", "恢复时刷新状态栏失败");
    ESP_RETURN_ON_ERROR(ui_router_refresh_current(), "ui_main", "恢复时刷新当前页面失败");
    return ui_pomodoro_banner_sync(ui_router_get_current());
}

/**
 * @brief 在调用方持有 LVGL 锁时同步处理一条 UI 消息
 */
esp_err_t ui_main_handle_message(const ui_msg_t *msg)
{
    ESP_RETURN_ON_FALSE(msg != NULL, ESP_ERR_INVALID_ARG, "ui_main", "UI 消息为空");
    esp_err_t err = ESP_OK;
    switch (msg->type)
    {
        case UI_MSG_SWITCH_PAGE:
            err = ui_router_switch_to(msg->page, (ui_nav_dir_t) msg->param);
            if (err == ESP_OK)
            {
                err = ui_pomodoro_banner_sync(ui_router_get_current());
            }
            break;
        case UI_MSG_STATUS_BAR_UPDATE:
            err = refresh_status_bar();
            break;
        case UI_MSG_STATUS_UPDATE: {
            err = refresh_status_bar();
            if (err == ESP_OK)
            {
                err = ui_router_refresh_current();
            }
            break;
        }
        case UI_MSG_POMODORO_UPDATE: {
            err = refresh_status_bar();
            if (err == ESP_OK && ui_router_get_current() == PRESENTATION_PAGE_POMODORO)
            {
                err = ui_router_refresh_current();
            }
            if (err == ESP_OK)
            {
                err = ui_pomodoro_banner_sync(ui_router_get_current());
            }
            break;
        }
        case UI_MSG_OTA_UPDATE:
            err = ui_router_refresh_current();
            break;
        case UI_MSG_SETTINGS_ACTION:
            err = ui_settings_page_handle_action((presentation_settings_action_t) msg->param);
            break;
        case UI_MSG_NONE:
            break;
        default:
            err = ESP_ERR_NOT_SUPPORTED;
            break;
    }
    if (msg->type != UI_MSG_NONE && (err == ESP_OK || msg->type == UI_MSG_SETTINGS_ACTION))
    {
        const esp_err_t refresh_error = ui_platform_lvgl_request_refresh();
        if (err == ESP_OK)
        {
            err = refresh_error;
        }
    }
    return err;
}

lv_obj_t *ui_main_get_status_bar(void)
{
    return s_status_bar;
}
