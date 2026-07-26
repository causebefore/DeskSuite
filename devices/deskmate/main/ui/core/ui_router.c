/*
 * 文件职责：按需创建 LVGL Screen，统一处理顶层页面切换、释放与当前页刷新。
 */
#include "ui_router.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "ui_calendar_page.h"
#include "ui_home_page.h"
#include "ui_mail_page.h"
#include "ui_quota_page.h"
#include "ui_runtime.h"
#include "ui_settings_page.h"
#include "ui_test_page.h"
#include "ui_weather_page.h"
#include "ui_voice_page.h"

#define UI_PAGE_CONTENT_HEIGHT  268
#define UI_PAGE_ENTER_OFFSET_PX 16

static const char *TAG             = "ui_router";

static ui_page_id_t s_current_page = PRESENTATION_PAGE_COUNT;
static lv_obj_t    *s_current_container;
static lv_obj_t    *s_page_screens[PRESENTATION_PAGE_COUNT];
static lv_obj_t    *s_transition_container;

/** @brief 把轻量页面进入动画的当前值写入内容容器 X 坐标 */
static void set_container_x(void *object, int32_t value)
{
    lv_obj_set_x((lv_obj_t *) object, value);
}

/** @brief 向 Application 上报当前目标页的视觉切换已经完成 */
static void emit_screen_loaded(ui_page_id_t page)
{
    const ui_user_intent_t intent = {
        .id   = UI_USER_INTENT_SCREEN_LOADED,
        .page = page,
    };
    const esp_err_t error = ui_runtime_emit_user_intent(&intent);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "上报 Screen 加载完成失败: page=%u err=%s", (unsigned) page, esp_err_to_name(error));
    }
}

/** @brief 页面进入动画完成后，仅为仍然有效的当前目标页解除输入门控 */
static void on_page_enter_completed(lv_anim_t *animation)
{
    lv_obj_t          *container = (lv_obj_t *) animation->var;
    const ui_page_id_t page      = (ui_page_id_t) (uintptr_t) lv_anim_get_user_data(animation);
    if (container != s_transition_container)
    {
        return;
    }

    s_transition_container = NULL;
    if (container == s_current_container && page == s_current_page)
    {
        emit_screen_loaded(page);
    }
}

/** @brief 动画被对象删除或新切换打断时清空 Router 保存的借用句柄 */
static void on_page_enter_deleted(lv_anim_t *animation)
{
    if (animation->var == s_transition_container)
    {
        s_transition_container = NULL;
    }
}

/** @brief 取消尚未完成的页面进入动画并把旧内容恢复到稳定位置 */
static void cancel_page_enter_animation(void)
{
    lv_obj_t *container    = s_transition_container;
    s_transition_container = NULL;
    if (container == NULL)
    {
        return;
    }

    (void) lv_anim_delete(container, set_container_x);
    lv_obj_set_x(container, 0);
}

/** @brief 播放小位移方向动画；启动失败时同步收敛为已完成状态 */
static void start_page_enter_animation(lv_obj_t *container, ui_page_id_t page, ui_nav_dir_t dir)
{
    if (dir == PRESENTATION_NAV_DIR_NONE || CONFIG_DESKMATE_UI_PAGE_ENTER_ANIM_MS == 0)
    {
        emit_screen_loaded(page);
        return;
    }

    const int32_t start_x = dir == PRESENTATION_NAV_DIR_FORWARD ? UI_PAGE_ENTER_OFFSET_PX : -UI_PAGE_ENTER_OFFSET_PX;
    lv_obj_set_x(container, start_x);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, container);
    lv_anim_set_values(&animation, start_x, 0);
    lv_anim_set_duration(&animation, CONFIG_DESKMATE_UI_PAGE_ENTER_ANIM_MS);
    lv_anim_set_exec_cb(&animation, set_container_x);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&animation, on_page_enter_completed);
    lv_anim_set_deleted_cb(&animation, on_page_enter_deleted);
    lv_anim_set_user_data(&animation, (void *) (uintptr_t) page);

    s_transition_container = container;
    if (lv_anim_start(&animation) == NULL)
    {
        s_transition_container = NULL;
        lv_obj_set_x(container, 0);
        emit_screen_loaded(page);
    }
}

/** @brief 清空指定页面模块对已删除控件树的借用句柄 */
static void reset_page(ui_page_id_t page)
{
    switch (page)
    {
        case PRESENTATION_PAGE_HOME:
            ui_home_page_deinit();
            break;
        case PRESENTATION_PAGE_WEATHER:
            ui_weather_page_deinit();
            break;
        case PRESENTATION_PAGE_SETTINGS:
            ui_settings_page_deinit();
            break;
        default:
            break;
    }
}

/** @brief 在目标 Screen 的内容容器内创建并首次填充指定页面 */
static esp_err_t show_page(ui_page_id_t page, lv_obj_t *container)
{
    switch (page)
    {
        case PRESENTATION_PAGE_HOME:
            return ui_home_page_show(container);
        case PRESENTATION_PAGE_WEATHER:
            return ui_weather_page_show(container);
        case PRESENTATION_PAGE_VOICE:
            return ui_voice_page_show(container);
        case PRESENTATION_PAGE_CALENDAR:
            return ui_calendar_page_show(container);
        case PRESENTATION_PAGE_MAIL:
            return ui_mail_page_show(container);
        case PRESENTATION_PAGE_QUOTA:
            return ui_quota_page_show(container);
        case PRESENTATION_PAGE_SETTINGS:
            return ui_settings_page_show(container);
        case PRESENTATION_PAGE_TEST:
            return ui_test_page_show(container);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

/** @brief 使用当前 Screen 的内容容器刷新指定页面 */
static esp_err_t update_page(ui_page_id_t page, lv_obj_t *container)
{
    switch (page)
    {
        case PRESENTATION_PAGE_HOME:
            return ui_home_page_update(container);
        case PRESENTATION_PAGE_WEATHER:
            return ui_weather_page_update(container);
        case PRESENTATION_PAGE_VOICE:
            return ui_voice_page_update(container);
        case PRESENTATION_PAGE_CALENDAR:
            return ui_calendar_page_update(container);
        case PRESENTATION_PAGE_MAIL:
            return ui_mail_page_update(container);
        case PRESENTATION_PAGE_QUOTA:
            return ui_quota_page_update(container);
        case PRESENTATION_PAGE_SETTINGS:
            return ui_settings_page_update(container);
        case PRESENTATION_PAGE_TEST:
            return ui_test_page_update(container);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

/**
 * @brief 仅当被删除 Screen 仍是该页面资源的所有者时清理页面静态句柄
 *
 * 同一页面的旧 Screen 可能晚于新 Screen 删除；实例校验可避免旧回调清掉新页面的
 * Timer、Group 或控件句柄。
 */
static void on_screen_event(lv_event_t *event)
{
    const ui_page_id_t page   = (ui_page_id_t) (uintptr_t) lv_event_get_user_data(event);
    lv_obj_t          *screen = lv_event_get_current_target(event);
    if ((unsigned) page >= PRESENTATION_PAGE_COUNT || s_page_screens[page] != screen)
    {
        return;
    }

    s_page_screens[page] = NULL;
    reset_page(page);
}

/** @brief 创建无边框顶层 Screen 及位于状态栏下方的页面内容容器 */
static esp_err_t create_screen(lv_obj_t **out_screen, lv_obj_t **out_container)
{
    ESP_RETURN_ON_FALSE(out_screen != NULL && out_container != NULL, ESP_ERR_INVALID_ARG, TAG, "Screen 输出参数为空");
    lv_obj_t *screen = lv_obj_create(NULL);
    ESP_RETURN_ON_FALSE(screen != NULL, ESP_ERR_NO_MEM, TAG, "创建顶层 Screen 失败");
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *container = lv_obj_create(screen);
    if (container == NULL)
    {
        lv_obj_delete(screen);
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(container);
    lv_obj_set_pos(container, 0, 32);
    lv_obj_set_size(container, LV_PCT(100), UI_PAGE_CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(container, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(container, lv_color_black(), 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    *out_screen    = screen;
    *out_container = container;
    return ESP_OK;
}

esp_err_t ui_router_init(void)
{
    ESP_RETURN_ON_ERROR(ui_home_page_init(), TAG, "主页初始化失败");
    ESP_RETURN_ON_ERROR(ui_weather_page_init(), TAG, "天气页初始化失败");
    ESP_RETURN_ON_ERROR(ui_calendar_page_init(), TAG, "日历页初始化失败");
    ESP_RETURN_ON_ERROR(ui_voice_page_init(), TAG, "语音页初始化失败");
    ESP_RETURN_ON_ERROR(ui_mail_page_init(), TAG, "邮箱页初始化失败");
    ESP_RETURN_ON_ERROR(ui_quota_page_init(), TAG, "限额页初始化失败");
    ESP_RETURN_ON_ERROR(ui_settings_page_init(), TAG, "设置页初始化失败");
    ESP_RETURN_ON_ERROR(ui_test_page_init(), TAG, "测试页初始化失败");
    s_current_page         = PRESENTATION_PAGE_COUNT;
    s_current_container    = NULL;
    s_transition_container = NULL;
    memset(s_page_screens, 0, sizeof(s_page_screens));
    return ESP_OK;
}

void ui_router_deinit(void)
{
    cancel_page_enter_animation();
    lv_anim_delete_all();
    ui_home_page_deinit();
    ui_weather_page_deinit();
    ui_settings_page_deinit();
    s_current_page         = PRESENTATION_PAGE_COUNT;
    s_current_container    = NULL;
    s_transition_container = NULL;
    memset(s_page_screens, 0, sizeof(s_page_screens));
}

esp_err_t ui_router_switch_to(ui_page_id_t page, ui_nav_dir_t dir)
{
    if ((unsigned) page >= PRESENTATION_PAGE_COUNT || (unsigned) dir > PRESENTATION_NAV_DIR_BACKWARD)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (page == s_current_page && s_current_container != NULL)
    {
        const esp_err_t error = update_page(page, s_current_container);
        if (error == ESP_OK && s_transition_container != s_current_container)
        {
            emit_screen_loaded(page);
        }
        return error;
    }

    cancel_page_enter_animation();
    lv_obj_t *screen    = NULL;
    lv_obj_t *container = NULL;
    ESP_RETURN_ON_ERROR(create_screen(&screen, &container), TAG, "创建页面 Screen 失败");
    const esp_err_t error = show_page(page, container);
    if (error != ESP_OK)
    {
        reset_page(page);
        lv_obj_delete(screen);
        return error;
    }

    lv_obj_add_event_cb(screen, on_screen_event, LV_EVENT_DELETE, (void *) (uintptr_t) page);
    s_page_screens[page] = screen;
    s_current_page       = page;
    s_current_container  = container;

    /*
     * 面板约 45 ms 才能稳定提交一帧；400 px 双 Screen MOVE 在 200 ms 内只有约
     * 5 个可见位置。先立即切换并释放旧 Screen，再仅让新内容移动 16 px。
     */
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_NONE, 0U, 0U, true);
    start_page_enter_animation(container, page, dir);
    return ESP_OK;
}

esp_err_t ui_router_refresh_current(void)
{
    if ((unsigned) s_current_page >= PRESENTATION_PAGE_COUNT || s_current_container == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return update_page(s_current_page, s_current_container);
}
