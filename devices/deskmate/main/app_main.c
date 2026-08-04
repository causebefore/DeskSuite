/*
 * 文件职责：作为 Composition Root 装配 Application、Presentation、UI 和运行时能力。
 * 调用方：main.c 的 app_main。
 */
#include "app_main.h"
#include "app_environment.h"
#include "app_key.h"
#include "app_network.h"
#include "app_ota.h"
#include "app_page.h"
#include "app_pomodoro.h"
#include "app_power.h"
#include "app_settings.h"
#include "app_voice.h"
#include "app_web_console.h"
#include "audio_processor_service.h"
#include "audio_service.h"
#include "button_service.h"
#include "calendar_presenter.h"
#include "dashboard_store.h"
#include "device_audio.h"
#include "device_battery.h"
#include "device_button.h"
#include "device_environment.h"
#include "environment_service.h"
#include "firmware_ota.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "home_presenter.h"
#include "mail_presenter.h"
#include "ota_presenter.h"
#include "pomodoro_presenter.h"
#include "quota_presenter.h"
#include "rtc_service.h"
#include "sdkconfig.h"
#include "settings_presenter.h"
#include "status_bar_presenter.h"
#include "system_presenter.h"
#include "ui_runtime.h"
#include "voice_presenter.h"
#include "voice_service.h"
#include "weather_presenter.h"
#include "web_console_presenter.h"
#include "web_console_service.h"

#define APP_UI_START_TIMEOUT_MS         5000
#define APP_UI_STOP_TIMEOUT_MS          5000
#define APP_ENVIRONMENT_STOP_TIMEOUT_MS 1000
#define APP_POWER_STOP_TIMEOUT_MS       1000
#define APP_VOICE_LIFECYCLE_TIMEOUT_MS  3000
#define APP_AUDIO_STOP_TIMEOUT_MS       3000
#define APP_POMODORO_STOP_TIMEOUT_MS    1000

static const char *TAG = "app_main";

/** @brief 把 UI Task 上报的窄意图转交给对应 Application，并返回异步请求接受结果 */
static esp_err_t app_main_ui_user_intent_callback(const ui_user_intent_t *intent, ui_user_intent_result_t *out_result,
                                                  void *context)
{
    (void) context;
    if (intent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    switch (intent->id)
    {
        case UI_USER_INTENT_SCREEN_LOADED:
            app_page_reconcile_screen_loaded(intent->page);
            return ESP_OK;
        case UI_USER_INTENT_SETTINGS_MENU_CLOSED:
            return app_settings_reset();
        case UI_USER_INTENT_SETTINGS_START_PORTAL:
            return app_settings_request_portal();
        case UI_USER_INTENT_SETTINGS_OTA_CHECK:
            return app_ota_request_check();
        case UI_USER_INTENT_SETTINGS_OTA_INSTALL:
            return app_ota_request_install();
        case UI_USER_INTENT_SETTINGS_OTA_DISCARD:
            return app_ota_clear_pending_update();
        case UI_USER_INTENT_SETTINGS_START_WEB_CONSOLE:
            return app_web_console_request_start();
        case UI_USER_INTENT_SETTINGS_STOP_WEB_CONSOLE:
            return app_web_console_request_stop();
        case UI_USER_INTENT_POMODORO_SETTINGS_SAVE: {
            if (out_result == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }
            app_pomodoro_snapshot_t snapshot;
            esp_err_t               error = app_pomodoro_get_snapshot_copy(&snapshot);
            if (error != ESP_OK)
            {
                return error;
            }
            app_pomodoro_settings_update_t update = {
                .settings         = snapshot.settings,
                .expected_version = intent->pomodoro_settings.expected_version,
            };
            update.settings.focus_minutes       = intent->pomodoro_settings.focus_minutes;
            update.settings.short_break_minutes = intent->pomodoro_settings.short_break_minutes;
            update.settings.long_break_minutes  = intent->pomodoro_settings.long_break_minutes;
            update.settings.long_break_interval = intent->pomodoro_settings.long_break_interval;
            uint64_t request_id                 = 0U;
            error                               = app_pomodoro_request_update_settings_copy(&update, &request_id);
            if (error == ESP_OK)
            {
                out_result->request_id = request_id;
            }
            return error;
        }
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

/** @brief 消费 RTC Service 事件；周期 RTC 告警不重置用户无活动窗口 */
static void app_main_rtc_event_callback(const rtc_service_event_t *event, void *context)
{
    (void) context;
    if (event->id == RTC_SERVICE_EVENT_ALARM_TRIGGERED)
    {
        ESP_LOGI(TAG, "收到 RTC 告警事件，累计=%lu", (unsigned long) event->alarm_count);
        return;
    }
    ESP_LOGE(TAG, "RTC 告警事件处理失败: %s", esp_err_to_name(event->result));
}

static esp_err_t ensure_default_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE)
    {
        return ESP_OK;
    }
    return err;
}

/** @brief 启动失败时按 stop → deinit 顺序完整回滚 UI Runtime */
static void rollback_ui_runtime(void)
{
    ui_runtime_state_t state = ui_runtime_get_state();
    if (state == UI_RUNTIME_STATE_RUNNING)
    {
        const esp_err_t stop_error = ui_runtime_stop(APP_UI_STOP_TIMEOUT_MS);
        if (stop_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚停止 UI Runtime 失败: %s", esp_err_to_name(stop_error));
        }
        state = ui_runtime_get_state();
    }

    if (state == UI_RUNTIME_STATE_STOPPED || state == UI_RUNTIME_STATE_FAILED)
    {
        const esp_err_t deinit_error = ui_runtime_deinit();
        if (deinit_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚反初始化 UI Runtime 失败: %s", esp_err_to_name(deinit_error));
        }
    }
    else
    {
        ESP_LOGE(TAG, "UI Runtime 状态不允许安全回滚: state=%d", (int) state);
    }
}

/**
 * @brief 启动失败时停止并反初始化语音 Application
 *
 * @return true Application 已释放，可以继续释放底层音频依赖；false 必须保留依赖
 */
static bool rollback_voice_application(void)
{
    app_voice_status_t status       = { 0 };
    const esp_err_t    status_error = app_voice_get_status_copy(&status);
    if (status_error != ESP_OK)
    {
        const esp_err_t deinit_error = app_voice_deinit();
        if (deinit_error == ESP_OK)
        {
            return true;
        }
        ESP_LOGE(TAG,
                 "读取语音 Application 回滚状态失败且无法直接反初始化，保留音频依赖: status=%s deinit=%s",
                 esp_err_to_name(status_error),
                 esp_err_to_name(deinit_error));
        return false;
    }
    if (status.state == APP_VOICE_STATE_RUNNING)
    {
        const esp_err_t error = app_voice_stop(APP_VOICE_LIFECYCLE_TIMEOUT_MS);
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚停止语音 Runtime 失败: %s", esp_err_to_name(error));
            return false;
        }
    }

    if (app_voice_get_status_copy(&status) != ESP_OK || status.state != APP_VOICE_STATE_STOPPED)
    {
        ESP_LOGE(TAG, "语音 Application 未到达安全回滚态，保留音频依赖");
        return false;
    }

    const esp_err_t error = app_voice_deinit();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "回滚反初始化语音 Application 失败，保留音频依赖: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}

/**
 * @brief 启动失败时停止并反初始化 Power Application
 *
 * @param[in] started 本次失败前是否已成功启动
 * @return true Power Task 不再引用下层运行时；false 必须保留其依赖
 */
static bool rollback_power_application(bool started)
{
    if (started)
    {
        const esp_err_t stop_error = app_power_stop(APP_POWER_STOP_TIMEOUT_MS);
        if (stop_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚停止轻睡眠 Application 失败，保留其依赖: %s", esp_err_to_name(stop_error));
            return false;
        }
    }
    const esp_err_t deinit_error = app_power_deinit();
    if (deinit_error != ESP_OK)
    {
        ESP_LOGE(TAG, "回滚反初始化轻睡眠 Application 失败，保留其依赖: %s", esp_err_to_name(deinit_error));
        return false;
    }
    return true;
}

/** @brief 启动失败时尽力把番茄钟 Task 收敛回停止态 */
static void rollback_pomodoro_runtime(void)
{
    const esp_err_t error = app_pomodoro_stop(APP_POMODORO_STOP_TIMEOUT_MS);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "回滚停止番茄钟 Task 失败: %s", esp_err_to_name(error));
    }
}

/**
 * @brief 由 Composition Root 按 Device → Service 顺序装配音频能力
 *
 * 任一步失败都会只回滚本函数已经成功创建的资源，Service 不互相初始化，也不拥有
 * device_audio 的生命周期。
 */
static esp_err_t init_audio_runtime(void)
{
    const device_audio_config_t device_config = {
        .sample_rate_hz = CONFIG_DESKMATE_AUDIO_SAMPLE_RATE_HZ,
        .initial_volume = CONFIG_DESKMATE_AUDIO_DEFAULT_VOLUME,
        .input_gain_db  = CONFIG_DESKMATE_AUDIO_DEFAULT_GAIN_DB,
    };
    bool      device_initialized    = false;
    bool      audio_initialized     = false;
    bool      audio_started         = false;
    bool      processor_initialized = false;
    bool      voice_initialized     = false;
    esp_err_t ret                   = device_audio_init(&device_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 Device 音频能力失败: %s", esp_err_to_name(ret));
        return ret;
    }
    device_initialized = true;

    ret                = audio_service_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化音频 Service 失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    audio_initialized = true;

    ret               = audio_service_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启动音频输出 Service 失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    audio_started = true;

    ret           = audio_processor_service_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化音频处理 Service 失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    processor_initialized = true;

    ret                   = voice_service_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化语音 Service 失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    voice_initialized = true;

    return ESP_OK;

cleanup:
    if (voice_initialized)
    {
        const esp_err_t cleanup_error = voice_service_deinit();
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚语音 Service 失败，保留其依赖: %s", esp_err_to_name(cleanup_error));
            return ret;
        }
    }
    if (processor_initialized || audio_processor_service_is_initialized())
    {
        const esp_err_t cleanup_error = audio_processor_service_deinit();
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚音频处理 Service 失败，保留其依赖: %s", esp_err_to_name(cleanup_error));
            return ret;
        }
    }
    if (audio_started)
    {
        const esp_err_t cleanup_error = audio_service_stop(APP_AUDIO_STOP_TIMEOUT_MS);
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚停止音频输出 Service 失败，保留其资源: %s", esp_err_to_name(cleanup_error));
            return ret;
        }
    }
    if (audio_initialized)
    {
        const esp_err_t cleanup_error = audio_service_deinit();
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚音频 Service 失败，保留 Device: %s", esp_err_to_name(cleanup_error));
            return ret;
        }
    }
    if (device_initialized)
    {
        const esp_err_t cleanup_error = device_audio_deinit();
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚 Device 音频能力失败: %s", esp_err_to_name(cleanup_error));
        }
    }
    return ret;
}

/** @brief 顶层启动失败时按 Voice → Processor → Audio → Device 顺序释放音频运行时。 */
static void rollback_audio_runtime(void)
{
    voice_service_status_t voice_status;
    esp_err_t              status_error = voice_service_get_status_copy(&voice_status);
    if (status_error == ESP_OK)
    {
        if (voice_status.state == VOICE_SERVICE_STATE_RUNNING && !voice_status.session_busy)
        {
            (void) voice_service_stop();
        }
        status_error = voice_service_get_status_copy(&voice_status);
        if (status_error != ESP_OK || voice_status.state != VOICE_SERVICE_STATE_STOPPED)
        {
            ESP_LOGE(TAG, "语音 Service 未到达安全回滚态，保留音频依赖");
            return;
        }
        const esp_err_t error = voice_service_deinit();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚语音 Service 失败: %s", esp_err_to_name(error));
            return;
        }
    }

    audio_processor_service_status_t processor_status;
    status_error = audio_processor_service_get_status_copy(&processor_status);
    if (status_error == ESP_OK)
    {
        if ((processor_status.state == AUDIO_PROCESSOR_STATE_RUNNING
             || processor_status.state == AUDIO_PROCESSOR_STATE_CLEANUP_FAILED)
            && processor_status.capture_state == AUDIO_PROCESSOR_CAPTURE_IDLE)
        {
            (void) audio_processor_service_stop(APP_VOICE_LIFECYCLE_TIMEOUT_MS);
        }
        status_error = audio_processor_service_get_status_copy(&processor_status);
        if (status_error != ESP_OK || processor_status.state != AUDIO_PROCESSOR_STATE_STOPPED)
        {
            ESP_LOGE(TAG, "音频处理 Service 未到达安全回滚态，保留底层依赖");
            return;
        }
        const esp_err_t error = audio_processor_service_deinit();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚音频处理 Service 失败: %s", esp_err_to_name(error));
            return;
        }
    }

    audio_service_status_t audio_status;
    status_error = audio_service_get_status_copy(&audio_status);
    if (status_error == ESP_OK)
    {
        if (audio_status.state == AUDIO_SERVICE_STATE_RUNNING
            || audio_status.state == AUDIO_SERVICE_STATE_CLEANUP_FAILED)
        {
            (void) audio_service_stop(APP_AUDIO_STOP_TIMEOUT_MS);
        }
        status_error = audio_service_get_status_copy(&audio_status);
        if (status_error != ESP_OK || audio_status.state != AUDIO_SERVICE_STATE_STOPPED)
        {
            ESP_LOGE(TAG, "音频输出 Service 未到达安全回滚态，保留 Device");
            return;
        }
        const esp_err_t error = audio_service_deinit();
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚音频输出 Service 失败: %s", esp_err_to_name(error));
            return;
        }
    }

    const esp_err_t device_error = device_audio_deinit();
    if (device_error != ESP_OK && device_error != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "回滚 Device 音频能力失败: %s", esp_err_to_name(device_error));
    }
}

/**
 * @brief 由 Composition Root 装配输入与环境链路的 Device 和 Service
 */
static esp_err_t init_input_environment_runtime(void)
{
    const button_service_config_t button_config = {
        .scan_period_ms = CONFIG_DESKMATE_KEY_POLL_PERIOD_MS,
    };
    bool      button_device_initialized       = false;
    bool      battery_device_initialized      = false;
    bool      environment_device_initialized  = false;
    bool      button_service_initialized      = false;
    bool      environment_service_initialized = false;
    esp_err_t ret = device_button_init(CONFIG_DESKMATE_KEY_DEBOUNCE_MS, CONFIG_DESKMATE_KEY_LONG_PRESS_MS);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化按键 Device 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    button_device_initialized = true;

    ret                       = button_service_init(&button_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化按键 Service 失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    button_service_initialized = true;

    ret                        = device_battery_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化电池 Device 失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    battery_device_initialized = true;

    ret                        = device_environment_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化环境 Device 失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    environment_device_initialized = true;

    ret                            = environment_service_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化环境 Service 失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    environment_service_initialized = true;
    return ESP_OK;

cleanup:
    if (environment_service_initialized)
    {
        (void) environment_service_deinit();
    }
    if (environment_device_initialized)
    {
        (void) device_environment_deinit();
    }
    if (battery_device_initialized)
    {
        (void) device_battery_deinit();
    }
    if (button_service_initialized)
    {
        (void) button_service_deinit();
    }
    if (button_device_initialized)
    {
        (void) device_button_deinit();
    }
    return ret;
}

/**
 * @brief 按依赖顺序初始化同步运行时能力。
 *
 * 不再通过 Service descriptor 隐式排序；依赖关系与实际初始化顺序在此处可见。
 */
static esp_err_t init_runtime_capabilities(void)
{
    ESP_RETURN_ON_ERROR(dashboard_store_init(), TAG, "初始化 Dashboard Store 失败");
    ESP_RETURN_ON_ERROR(init_input_environment_runtime(), TAG, "初始化输入与环境运行时失败");
    ESP_RETURN_ON_ERROR(rtc_service_init(), TAG, "初始化 RTC Service 失败");
    ESP_RETURN_ON_ERROR(rtc_service_set_event_callback_borrow(app_main_rtc_event_callback, NULL),
                        TAG,
                        "注册 RTC Service 事件回调失败");
    return ESP_OK;
}

/**
 * @brief 在顶层初始化失败时，仅于安全终态反初始化网页控制台 Service
 */
static void rollback_web_console_service_init(void)
{
    web_console_service_status_t status;
    const esp_err_t              status_error = web_console_service_get_status_copy(&status);
    if (status_error != ESP_OK)
    {
        ESP_LOGE(TAG, "读取网页控制台 Service 回滚状态失败: %s", esp_err_to_name(status_error));
        return;
    }

    if (status.state == WEB_CONSOLE_SERVICE_STATE_UNINITIALIZED)
    {
        return;
    }
    if (status.state != WEB_CONSOLE_SERVICE_STATE_INITIALIZED)
    {
        ESP_LOGE(TAG, "网页控制台 Service 未处于可反初始化终态，保留资源: state=%d", (int) status.state);
        return;
    }

    const esp_err_t cleanup_error = web_console_service_deinit();
    if (cleanup_error != ESP_OK)
    {
        ESP_LOGE(TAG, "回滚网页控制台 Service 初始化失败: %s", esp_err_to_name(cleanup_error));
    }
}

/**
 * @brief 初始化只负责 View Model 转换和 UI 通知的 Presentation 模块
 */
static esp_err_t init_presenters(void)
{
    ESP_RETURN_ON_ERROR(status_bar_presenter_init(), TAG, "状态栏 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(weather_presenter_init(), TAG, "天气页 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(calendar_presenter_init(), TAG, "日历页 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(mail_presenter_init(), TAG, "邮箱页 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(quota_presenter_init(), TAG, "限额页 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(home_presenter_init(), TAG, "首页 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(settings_presenter_init(), TAG, "设置页 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(system_presenter_init(), TAG, "系统页 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(voice_presenter_init(), TAG, "语音页 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(ota_presenter_init(), TAG, "OTA Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(web_console_presenter_init(), TAG, "网页控制台 Presenter 初始化失败");
    ESP_RETURN_ON_ERROR(pomodoro_presenter_init(), TAG, "番茄钟 Presenter 初始化失败");
    return ESP_OK;
}

/**
 * @brief 初始化拥有产品策略和用户意图的 Application 模块
 */
static esp_err_t init_applications(void)
{
    esp_err_t error = app_voice_init();
    ESP_RETURN_ON_ERROR(error, TAG, "语音 Application 初始化失败");
    error = app_ota_init();
    if (error != ESP_OK)
    {
        (void) rollback_voice_application();
        ESP_LOGE(TAG, "OTA Application 初始化失败: %s", esp_err_to_name(error));
        return error;
    }
    error = app_key_init();
    if (error != ESP_OK)
    {
        (void) rollback_voice_application();
        ESP_LOGE(TAG, "按键策略初始化失败: %s", esp_err_to_name(error));
        return error;
    }
    return ESP_OK;
}

esp_err_t app_main_init(void)
{
    ESP_RETURN_ON_ERROR(ensure_default_event_loop(), TAG, "创建默认事件循环失败");
    ESP_RETURN_ON_ERROR(init_runtime_capabilities(), TAG, "初始化运行时能力失败");

    bool environment_initialized = false;
    bool ui_initialized          = false;
    bool pomodoro_initialized    = false;
    bool voice_app_initialized   = false;

    esp_err_t error              = app_network_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化网络 Application 失败: %s", esp_err_to_name(error));
        goto cleanup;
    }
    error = init_presenters();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 Presentation 失败: %s", esp_err_to_name(error));
        goto cleanup;
    }
    error = init_applications();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 Application 失败: %s", esp_err_to_name(error));
        goto cleanup;
    }
    voice_app_initialized = true;
    error                 = app_pomodoro_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "番茄钟 Application 初始化失败: %s", esp_err_to_name(error));
        goto cleanup;
    }
    pomodoro_initialized = true;
    error                = app_web_console_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "网页控制台 Application 初始化失败: %s", esp_err_to_name(error));
        goto cleanup;
    }
    error = app_environment_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化环境 Application 失败: %s", esp_err_to_name(error));
        goto cleanup;
    }
    environment_initialized = true;

    error                   = ui_runtime_init();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 UI Task 失败: %s", esp_err_to_name(error));
        goto cleanup;
    }
    ui_initialized = true;

    error          = ui_runtime_set_user_intent_callback_borrow(app_main_ui_user_intent_callback, NULL);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "注册 UI 用户意图回调失败: %s", esp_err_to_name(error));
        goto cleanup;
    }

    const app_power_config_t power_config = {
#ifdef CONFIG_DESKMATE_LIGHT_SLEEP_ENABLED
        .automatic_light_sleep_enabled = true,
#else
        .automatic_light_sleep_enabled = false,
#endif
        .idle_timeout_ms     = CONFIG_DESKMATE_LIGHT_SLEEP_IDLE_TIMEOUT_SEC * 1000U,
        .refresh_interval_ms = CONFIG_DESKMATE_LIGHT_SLEEP_REFRESH_INTERVAL_SEC * 1000U,
        .retry_delay_ms      = CONFIG_DESKMATE_LIGHT_SLEEP_RETRY_DELAY_MS,
    };
    error = app_power_init(&power_config);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化轻睡眠 Application 失败: %s", esp_err_to_name(error));
        goto cleanup;
    }
    return ESP_OK;

cleanup:
    if (ui_initialized)
    {
        const esp_err_t cleanup_error = ui_runtime_deinit();
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚 UI Task 失败: %s", esp_err_to_name(cleanup_error));
        }
    }
    if (environment_initialized)
    {
        const esp_err_t cleanup_error = app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚环境 Task 失败: %s", esp_err_to_name(cleanup_error));
        }
    }
    rollback_web_console_service_init();
    if (pomodoro_initialized)
    {
        const esp_err_t cleanup_error = app_pomodoro_deinit();
        if (cleanup_error != ESP_OK)
        {
            ESP_LOGE(TAG, "回滚番茄钟 Application 失败: %s", esp_err_to_name(cleanup_error));
        }
    }
    if (voice_app_initialized)
    {
        (void) rollback_voice_application();
    }
    return error;
}

esp_err_t app_main_start(void)
{
    /*
     * 先让 UI Runtime 和首屏进入运行态，再初始化模型、AFE 等耗时能力。
     * 按键 Service 仍在全部页面依赖就绪后开放，避免输入进入未启动的产品能力。
     */
    esp_err_t error = ui_runtime_start(APP_UI_START_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "UI Task 启动失败: %s", esp_err_to_name(error));
        rollback_ui_runtime();
        if (rollback_power_application(false))
        {
            (void) rollback_voice_application();
        }
        (void) app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        return error;
    }

    error = app_page_dispatch_initial_presentation();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "派发首屏 UI 失败: %s", esp_err_to_name(error));
        rollback_ui_runtime();
        if (rollback_power_application(false))
        {
            (void) rollback_voice_application();
        }
        (void) app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        return error;
    }
    ESP_LOGI(TAG, "UI Runtime 已启动且首屏已派发，继续初始化后台运行期能力");

    error = rtc_service_start();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "启动 RTC Service 失败: %s", esp_err_to_name(error));
        rollback_ui_runtime();
        if (rollback_power_application(false))
        {
            (void) rollback_voice_application();
        }
        (void) app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        return error;
    }

    error = app_pomodoro_start();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "启动番茄钟 Task 失败: %s", esp_err_to_name(error));
        rollback_pomodoro_runtime();
        (void) rtc_service_stop(APP_POWER_STOP_TIMEOUT_MS);
        rollback_ui_runtime();
        if (rollback_power_application(false))
        {
            (void) rollback_voice_application();
        }
        (void) app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        return error;
    }

    error = init_audio_runtime();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化音频与语音运行时失败: %s", esp_err_to_name(error));
        rollback_pomodoro_runtime();
        (void) rtc_service_stop(APP_POWER_STOP_TIMEOUT_MS);
        rollback_ui_runtime();
        if (rollback_power_application(false))
        {
            (void) rollback_voice_application();
        }
        (void) app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        return error;
    }

    error = app_voice_start(APP_VOICE_LIFECYCLE_TIMEOUT_MS);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "启动语音 Runtime 失败: %s", esp_err_to_name(error));
        const bool power_released = rollback_power_application(false);
        if (power_released && rollback_voice_application())
        {
            rollback_audio_runtime();
        }
        rollback_pomodoro_runtime();
        (void) rtc_service_stop(APP_POWER_STOP_TIMEOUT_MS);
        rollback_ui_runtime();
        (void) app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        return error;
    }

    const esp_err_t startup_audio_error = app_pomodoro_request_play_completion_audio();
    if (startup_audio_error != ESP_OK)
    {
        ESP_LOGW(TAG, "提交开机提示音失败: %s", esp_err_to_name(startup_audio_error));
    }

    error = app_power_start();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "启动轻睡眠 Application 失败: %s", esp_err_to_name(error));
        const bool power_released = rollback_power_application(false);
        if (power_released && rollback_voice_application())
        {
            rollback_audio_runtime();
        }
        rollback_pomodoro_runtime();
        (void) rtc_service_stop(APP_POWER_STOP_TIMEOUT_MS);
        rollback_ui_runtime();
        (void) app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        return error;
    }

    error = button_service_start();
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "启动按键 Service 失败: %s", esp_err_to_name(error));
        const bool power_released = rollback_power_application(true);
        if (power_released && rollback_voice_application())
        {
            rollback_audio_runtime();
        }
        rollback_pomodoro_runtime();
        (void) rtc_service_stop(APP_POWER_STOP_TIMEOUT_MS);
        rollback_ui_runtime();
        (void) app_environment_deinit(APP_ENVIRONMENT_STOP_TIMEOUT_MS);
        return error;
    }

    const esp_err_t confirm_error = firmware_ota_confirm_running_image();
    if (confirm_error != ESP_OK)
    {
        ESP_LOGW(TAG, "确认当前 OTA 镜像本地启动健康失败: %s", esp_err_to_name(confirm_error));
    }

    app_network_set_dashboard_auto_sync_enabled(true);
    const esp_err_t sync_err = app_network_request_sync();
    if (sync_err != ESP_OK && sync_err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "启动 Dashboard 自动同步失败: %s", esp_err_to_name(sync_err));
    }
    else
    {
        ESP_LOGI(TAG, "Dashboard 首次同步已请求，服务端截止时间调度已启用");
    }
    return ESP_OK;
}
