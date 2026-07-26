/*
 * 文件职责：编排语音会话、实时网络租约和唤醒词产品策略。
 */
#include "app_voice.h"

#include "app_network.h"
#include "app_page.h"
#include "esp_event.h"
#include "esp_log.h"
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    #include "esp_timer.h"
    #include "audio_processor_service.h" /* AUDIO_PROCESSOR_EVENT + wake_arbiter_logic.h */
    #include "wake_arbiter_logic.h"
#endif
#include "voice_service.h"

static const char *TAG = "app_voice";
static uint32_t    s_network_lease_generation;

#define APP_VOICE_LEASE_TIMEOUT_MS 1500U

#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
static wake_arbiter_t s_wake_arbiter;
#endif

/* 对话时长与唤醒词冷却见 Kconfig: DeskMate Audio/Voice。 */

/**
 * @brief 释放当前 App 持有的实时语音网络租约
 */
static void release_network_lease(void)
{
    if (s_network_lease_generation == 0)
    {
        return;
    }
    const uint32_t generation  = s_network_lease_generation;
    s_network_lease_generation = 0;
    const esp_err_t err        = app_network_release_realtime_voice_lease(generation, APP_VOICE_LEASE_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "释放实时语音网络租约失败: generation=%lu err=%s",
                 (unsigned long) generation,
                 esp_err_to_name(err));
    }
}

/**
 * @brief 先取得实时网络租约，再启动一次语音对话
 */
static esp_err_t start_voice_chat(uint32_t duration_ms)
{
    uint32_t  generation = 0;
    esp_err_t err        = app_network_acquire_realtime_voice_lease(APP_VOICE_LEASE_TIMEOUT_MS, &generation);
    if (err != ESP_OK)
    {
        return err;
    }

    s_network_lease_generation = generation;
    err                        = voice_service_chat(duration_ms);
    if (err != ESP_OK)
    {
        release_network_lease();
    }
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
        release_network_lease();
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

    if (!wake_arbiter_handle(&s_wake_arbiter, now_ms, busy))
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
        (void) app_page_show(PRESENTATION_PAGE_VOICE, PRESENTATION_NAV_DIR_NONE);
    }
}
#endif

/**
 * @brief 初始化语音 Application 的租约收敛和唤醒策略
 */
esp_err_t app_voice_init(void)
{
    s_network_lease_generation = 0;

    esp_err_t err = esp_event_handler_register(VOICE_SERVICE_EVENT, ESP_EVENT_ANY_ID, on_voice_application_event, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "注册语音事件处理器失败: %s", esp_err_to_name(err));
        return err;
    }

#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    wake_arbiter_init(&s_wake_arbiter, CONFIG_DESKMATE_VOICE_WAKE_COOLDOWN_MS);
    err = esp_event_handler_register(AUDIO_PROCESSOR_EVENT, AUDIO_PROCESSOR_EVENT_WAKE, on_wake_event, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "注册唤醒事件处理器失败: %s", esp_err_to_name(err));
        return err;
    }
#endif

    /* 语音能力已由 App 运行时按依赖顺序初始化；此处只注册 App 事件。 */
    return ESP_OK;
}

/**
 * @brief 把语音页长按输入解释为开始或取消对话
 */
bool app_voice_consume_input(device_button_event_t key_event)
{
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
