/*
 * 文件职责：编排语音会话、实时网络租约和唤醒词产品策略。
 */
#include "app_voice.h"

#include "app_network.h"
#include "app_page.h"
#include "audio_processor_service.h"
#include "audio_service.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    #include "wake_arbiter_logic.h"
#endif
#include "voice_service.h"

static const char       *TAG          = "app_voice";
static portMUX_TYPE      s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_control_lock;
static app_voice_state_t s_state;
static uint32_t          s_network_lease_generation;
static esp_err_t         s_primary_error;
static esp_err_t         s_recovery_error;
static bool              s_voice_handler_registered;
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
static bool s_wake_handler_registered;
#endif

#define APP_VOICE_LEASE_TIMEOUT_MS 1500U

#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
static wake_arbiter_t s_wake_arbiter;
#endif

/* 对话时长与唤醒词冷却见 Kconfig: DeskMate Audio/Voice。 */

/**
 * @brief 释放当前 App 持有的实时语音网络租约
 */
static esp_err_t release_network_lease_locked(void)
{
    if (s_network_lease_generation == 0)
    {
        return ESP_OK;
    }
    const uint32_t  generation = s_network_lease_generation;
    const esp_err_t err        = app_network_release_realtime_voice_lease(generation, APP_VOICE_LEASE_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "释放实时语音网络租约失败: generation=%lu err=%s",
                 (unsigned long) generation,
                 esp_err_to_name(err));
        return err;
    }
    s_network_lease_generation = 0;
    return ESP_OK;
}

/** @brief 在短临界区内更新语音 Application 生命周期事实。 */
static void set_state(app_voice_state_t state, esp_err_t primary_error, esp_err_t recovery_error)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_state          = state;
    s_primary_error  = primary_error;
    s_recovery_error = recovery_error;
    taskEXIT_CRITICAL(&s_state_lock);
}

/** @brief 返回总生命周期期限内仍可使用的毫秒数，至少为 1ms。 */
static uint32_t remaining_ms(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0)
    {
        return 1U;
    }
    const uint64_t rounded_ms = ((uint64_t) remaining_us + 999U) / 1000U;
    return rounded_ms > UINT32_MAX ? UINT32_MAX : (uint32_t) rounded_ms;
}

/**
 * @brief 先取得实时网络租约，再启动一次语音对话
 */
static esp_err_t start_voice_chat(uint32_t duration_ms)
{
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_state_lock);
    const bool running = s_state == APP_VOICE_STATE_RUNNING;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!running)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t  generation = 0;
    esp_err_t err        = app_network_acquire_realtime_voice_lease(APP_VOICE_LEASE_TIMEOUT_MS, &generation);
    if (err != ESP_OK)
    {
        xSemaphoreGive(s_control_lock);
        return err;
    }

    protocol_backend_context_t backend;
    err = app_network_get_backend_context_copy(&backend);
    if (err != ESP_OK)
    {
        (void) app_network_release_realtime_voice_lease(generation, APP_VOICE_LEASE_TIMEOUT_MS);
        xSemaphoreGive(s_control_lock);
        return err;
    }

    s_network_lease_generation = generation;
    err                        = voice_service_request_chat(&backend, duration_ms);
    if (err != ESP_OK)
    {
        (void) release_network_lease_locked();
    }
    xSemaphoreGive(s_control_lock);
    return err;
}

/**
 * @brief 在语音终态释放实时网络租约
 */
static void on_voice_application_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) data;

    if (id == VOICE_SERVICE_EVENT_DONE || id == VOICE_SERVICE_EVENT_CANCELLED || id == VOICE_SERVICE_EVENT_ERROR)
    {
        if (s_control_lock != NULL)
        {
            xSemaphoreTake(s_control_lock, portMAX_DELAY);
            (void) release_network_lease_locked();
            xSemaphoreGive(s_control_lock);
        }
    }
}

#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
/**
 * @brief 仲裁唤醒词事件并在取得网络租约后启动语音对话
 */
static void on_wake_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    (void) base;
    (void) id;
    (void) data;

    uint32_t now_ms = (uint32_t) (esp_timer_get_time() / 1000);
    bool     busy   = voice_service_is_busy();

    if (!wake_arbiter_consume_detection(&s_wake_arbiter, now_ms, busy))
    {
        ESP_LOGI(TAG, "唤醒被仲裁丢弃 (busy=%d)", (int) busy);
        return;
    }

    ESP_LOGI(TAG, "唤醒通过，触发语音对话");
    esp_err_t err = start_voice_chat(CONFIG_DESKMATE_VOICE_CHAT_DURATION_MS);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "唤醒触发对话失败: %s", esp_err_to_name(err));
        return;
    }

    /* 租约已生效后只切换本地 UI；页面状态由设备 Application 独立拥有。 */
    if (app_page_get_current() != PRESENTATION_PAGE_VOICE)
    {
        (void) app_page_navigate(PRESENTATION_PAGE_VOICE, PRESENTATION_NAV_DIR_NONE);
    }
}
#endif

/**
 * @brief 初始化语音 Application 的租约收敛和唤醒策略
 */
esp_err_t app_voice_init(void)
{
    if (s_control_lock != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_control_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_control_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建语音生命周期互斥锁失败");

    s_network_lease_generation = 0;
    s_primary_error            = ESP_OK;
    s_recovery_error           = ESP_OK;

    esp_err_t err = esp_event_handler_register(VOICE_SERVICE_EVENT, ESP_EVENT_ANY_ID, on_voice_application_event, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "注册语音事件处理器失败: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_control_lock);
        s_control_lock = NULL;
        return err;
    }
    s_voice_handler_registered = true;

#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    wake_arbiter_init(&s_wake_arbiter, CONFIG_DESKMATE_VOICE_WAKE_COOLDOWN_MS);
    err = esp_event_handler_register(AUDIO_PROCESSOR_EVENT, AUDIO_PROCESSOR_EVENT_WAKE, on_wake_event, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "注册唤醒事件处理器失败: %s", esp_err_to_name(err));
        (void) esp_event_handler_unregister(VOICE_SERVICE_EVENT, ESP_EVENT_ANY_ID, on_voice_application_event);
        s_voice_handler_registered = false;
        vSemaphoreDelete(s_control_lock);
        s_control_lock = NULL;
        return err;
    }
    s_wake_handler_registered = true;
#endif

    set_state(APP_VOICE_STATE_STOPPED, ESP_OK, ESP_OK);
    ESP_LOGI(TAG, "语音 Application 已初始化，Runtime 保持停止");
    return ESP_OK;
}

esp_err_t app_voice_start(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "语音 Runtime 启动超时无效");
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_state_lock);
    const app_voice_state_t state = s_state;
    taskEXIT_CRITICAL(&s_state_lock);
    if (state == APP_VOICE_STATE_RUNNING)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_OK;
    }
    if (state != APP_VOICE_STATE_STOPPED)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }

    set_state(APP_VOICE_STATE_STARTING, ESP_OK, ESP_OK);
    const int64_t deadline_us       = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    bool          audio_started     = false;
    bool          processor_started = false;
    bool          voice_started     = false;
    esp_err_t     primary_error     = audio_service_start();
    if (primary_error == ESP_OK)
    {
        audio_started = true;
        primary_error = audio_processor_service_start();
    }
    if (primary_error == ESP_OK)
    {
        processor_started = true;
        primary_error     = voice_service_start();
    }
    if (primary_error == ESP_OK)
    {
        voice_started = true;
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
        primary_error = audio_service_enable_input(true);
#endif
    }
    if (primary_error == ESP_OK)
    {
        set_state(APP_VOICE_STATE_RUNNING, ESP_OK, ESP_OK);
        xSemaphoreGive(s_control_lock);
        ESP_LOGI(TAG, "语音 Runtime 已启动");
        return ESP_OK;
    }

    esp_err_t recovery_error = ESP_OK;
    if (voice_started)
    {
        recovery_error = voice_service_stop();
    }
    if (processor_started)
    {
        const esp_err_t error = audio_processor_service_stop(remaining_ms(deadline_us));
        if (recovery_error == ESP_OK)
        {
            recovery_error = error;
        }
    }
    if (audio_started)
    {
        const esp_err_t error = audio_service_stop();
        if (recovery_error == ESP_OK)
        {
            recovery_error = error;
        }
    }
    set_state(recovery_error == ESP_OK ? APP_VOICE_STATE_STOPPED : APP_VOICE_STATE_FAILED,
              primary_error,
              recovery_error);
    xSemaphoreGive(s_control_lock);
    ESP_LOGE(TAG,
             "语音 Runtime 启动失败: primary=%s recovery=%s",
             esp_err_to_name(primary_error),
             esp_err_to_name(recovery_error));
    return primary_error;
}

esp_err_t app_voice_stop(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "语音 Runtime 停止超时无效");
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_state_lock);
    const app_voice_state_t state = s_state;
    taskEXIT_CRITICAL(&s_state_lock);
    if (state == APP_VOICE_STATE_STOPPED)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_OK;
    }
    if (state != APP_VOICE_STATE_RUNNING || voice_service_is_busy())
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_network_lease_generation != 0)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }

    set_state(APP_VOICE_STATE_STOPPING, ESP_OK, ESP_OK);
    const int64_t deadline_us   = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    esp_err_t     primary_error = voice_service_stop();
    if (primary_error != ESP_OK)
    {
        set_state(APP_VOICE_STATE_RUNNING, primary_error, ESP_OK);
        xSemaphoreGive(s_control_lock);
        return primary_error;
    }

    primary_error = audio_processor_service_stop(remaining_ms(deadline_us));
    if (primary_error != ESP_OK)
    {
        esp_err_t recovery_error = audio_processor_service_start();
        if (recovery_error == ESP_OK)
        {
            recovery_error = voice_service_start();
        }
        set_state(recovery_error == ESP_OK ? APP_VOICE_STATE_RUNNING : APP_VOICE_STATE_FAILED,
                  primary_error,
                  recovery_error);
        xSemaphoreGive(s_control_lock);
        return primary_error;
    }

    primary_error = audio_service_stop();
    if (primary_error != ESP_OK)
    {
        esp_err_t recovery_error = audio_service_start();
        if (recovery_error == ESP_OK)
        {
            recovery_error = audio_processor_service_start();
        }
        if (recovery_error == ESP_OK)
        {
            recovery_error = voice_service_start();
        }
        set_state(recovery_error == ESP_OK ? APP_VOICE_STATE_RUNNING : APP_VOICE_STATE_FAILED,
                  primary_error,
                  recovery_error);
        xSemaphoreGive(s_control_lock);
        return primary_error;
    }

    set_state(APP_VOICE_STATE_STOPPED, ESP_OK, ESP_OK);
    xSemaphoreGive(s_control_lock);
    ESP_LOGI(TAG, "语音 Runtime 已停止");
    return ESP_OK;
}

esp_err_t app_voice_deinit(void)
{
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_state_lock);
    const bool stopped = s_state == APP_VOICE_STATE_STOPPED;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!stopped || s_network_lease_generation != 0)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    if (s_wake_handler_registered)
    {
        const esp_err_t error =
            esp_event_handler_unregister(AUDIO_PROCESSOR_EVENT, AUDIO_PROCESSOR_EVENT_WAKE, on_wake_event);
        if (error != ESP_OK)
        {
            xSemaphoreGive(s_control_lock);
            ESP_LOGE(TAG, "注销唤醒事件处理器失败: %s", esp_err_to_name(error));
            return error;
        }
        s_wake_handler_registered = false;
    }
#endif
    if (s_voice_handler_registered)
    {
        const esp_err_t error =
            esp_event_handler_unregister(VOICE_SERVICE_EVENT, ESP_EVENT_ANY_ID, on_voice_application_event);
        if (error != ESP_OK)
        {
            xSemaphoreGive(s_control_lock);
            ESP_LOGE(TAG, "注销语音事件处理器失败: %s", esp_err_to_name(error));
            return error;
        }
        s_voice_handler_registered = false;
    }
    set_state(APP_VOICE_STATE_UNINITIALIZED, ESP_OK, ESP_OK);
    xSemaphoreGive(s_control_lock);
    vSemaphoreDelete(s_control_lock);
    s_control_lock = NULL;
    return ESP_OK;
}

esp_err_t app_voice_get_status_copy(app_voice_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "语音 Runtime 状态输出为空");
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_state_lock);
    const app_voice_state_t state          = s_state;
    const esp_err_t         primary_error  = s_primary_error;
    const esp_err_t         recovery_error = s_recovery_error;
    taskEXIT_CRITICAL(&s_state_lock);

    voice_service_status_t           voice_status     = { 0 };
    audio_processor_service_status_t processor_status = { 0 };
    audio_service_status_t           audio_status     = { 0 };
    const esp_err_t                  voice_error      = voice_service_get_status_copy(&voice_status);
    const esp_err_t                  processor_error  = audio_processor_service_get_status_copy(&processor_status);
    const esp_err_t                  audio_error      = audio_service_get_status_copy(&audio_status);
    if (voice_error != ESP_OK || processor_error != ESP_OK || audio_error != ESP_OK)
    {
        xSemaphoreGive(s_control_lock);
        return voice_error != ESP_OK ? voice_error : (processor_error != ESP_OK ? processor_error : audio_error);
    }

    *out_status = (app_voice_status_t) {
        .state          = state,
        .session_busy   = voice_status.session_busy,
        .processor_idle = processor_status.capture_state == AUDIO_PROCESSOR_CAPTURE_IDLE && processor_status.feed_parked
                          && processor_status.fetch_parked,
        .input_active   = audio_status.input_active,
        .output_active  = audio_status.output_active,
        .network_lease_held = s_network_lease_generation != 0,
        .primary_error      = primary_error,
        .recovery_error     = recovery_error,
    };
    xSemaphoreGive(s_control_lock);
    return ESP_OK;
}

/**
 * @brief 把语音页长按输入解释为开始或取消对话
 */
bool app_voice_consume_input(device_button_event_t key_event)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool running = s_state == APP_VOICE_STATE_RUNNING;
    taskEXIT_CRITICAL(&s_state_lock);
    if (!running)
    {
        return false;
    }
    if (app_page_get_current() != PRESENTATION_PAGE_VOICE)
    {
        return false;
    }

    if (key_event == DEVICE_BUTTON_EVENT_RIGHT_LONG)
    {
        if (voice_service_is_busy())
        {
            ESP_LOGI(TAG, "右键长按：取消当前语音对话");
            (void) voice_service_cancel();
            return true;
        }
        esp_err_t err = start_voice_chat(CONFIG_DESKMATE_VOICE_CHAT_DURATION_MS);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "启动语音对话失败: %s", esp_err_to_name(err));
        }
        return true;
    }

    return false;
}
