#include "audio_service.h"

#include "device_audio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "audio_service";

/* 默认音量见 Kconfig: DeskMate Audio/Voice */

typedef struct
{
    bool initialized;
    bool input_running;  /* 麦克风链路是否在跑 */
    bool output_running; /* 扬声器链路是否在跑 */
    bool muted;          /* 是否静音 */
    int  volume;         /* 真实音量值，静音时保留 */
} audio_service_ctx_t;

static audio_service_ctx_t s_ctx;
static SemaphoreHandle_t   s_lock;

esp_err_t audio_service_init(void)
{
    if (s_ctx.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(device_audio_get_sample_rate_hz() > 0U,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "Device 音频能力尚未初始化");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建音频互斥锁失败");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ctx.initialized = true;
    s_ctx.volume      = CONFIG_DESKMATE_AUDIO_DEFAULT_VOLUME;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "音频服务初始化完成");
    return ESP_OK;
}

esp_err_t audio_service_deinit(void)
{
    if (!s_ctx.initialized || s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t stop_error = audio_service_stop();
    if (stop_error != ESP_OK)
    {
        return stop_error;
    }

    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_ctx  = (audio_service_ctx_t) { 0 };
    ESP_LOGI(TAG, "音频服务已反初始化");
    return stop_error;
}

esp_err_t audio_service_enable_input(bool enable)
{
    if (!s_ctx.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool cur = s_ctx.input_running;
    xSemaphoreGive(s_lock);
    if (cur == enable)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(device_audio_enable_input(enable), TAG, "%s", enable ? "使能录音失败" : "关闭录音失败");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ctx.input_running = enable;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_service_enable_output(bool enable)
{
    if (!s_ctx.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool cur = s_ctx.output_running;
    xSemaphoreGive(s_lock);
    if (cur == enable)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(device_audio_enable_output(enable), TAG, "%s", enable ? "使能播放失败" : "关闭播放失败");
    if (enable)
    {
        /* 恢复音量（考虑静音状态） */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int  vol   = s_ctx.volume;
        bool muted = s_ctx.muted;
        xSemaphoreGive(s_lock);
        const esp_err_t volume_error = device_audio_set_output_volume(muted ? 0 : vol);
        if (volume_error != ESP_OK)
        {
            (void) device_audio_enable_output(false);
            return volume_error;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ctx.output_running = enable;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_service_stop(void)
{
    if (!s_ctx.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t input_err  = audio_service_enable_input(false);
    esp_err_t output_err = audio_service_enable_output(false);
    ESP_LOGI(TAG, "音频链路已停止");
    return input_err != ESP_OK ? input_err : output_err;
}

esp_err_t audio_service_write(const int16_t *data, size_t sample_count, size_t *out_written)
{
    ESP_RETURN_ON_FALSE(out_written != NULL, ESP_ERR_INVALID_ARG, TAG, "播放写入计数输出为空");
    *out_written = 0U;
    if (!s_ctx.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool output_running = s_ctx.output_running;
    xSemaphoreGive(s_lock);
    if (!output_running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return device_audio_write(data, sample_count, out_written);
}

esp_err_t audio_service_read(int16_t *dest, size_t sample_count, size_t *out_read)
{
    ESP_RETURN_ON_FALSE(out_read != NULL, ESP_ERR_INVALID_ARG, TAG, "录音读取计数输出为空");
    *out_read = 0U;
    if (!s_ctx.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool input_running = s_ctx.input_running;
    xSemaphoreGive(s_lock);
    if (!input_running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return device_audio_read(dest, sample_count, out_read);
}

esp_err_t audio_service_set_volume(int volume)
{
    if (!s_ctx.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(volume >= 0 && volume <= 100, ESP_ERR_INVALID_ARG, TAG, "音量超出 0～100");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool muted = s_ctx.muted;
    xSemaphoreGive(s_lock);

    /* 静音时底层设为 0，取消静音时恢复真实音量 */
    ESP_RETURN_ON_ERROR(device_audio_set_output_volume(muted ? 0 : volume), TAG, "设置设备音量失败");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ctx.volume = volume;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_service_set_muted(bool muted)
{
    if (!s_ctx.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_ctx.muted != muted;
    const int  volume  = s_ctx.volume;
    xSemaphoreGive(s_lock);

    if (!changed)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(device_audio_set_output_volume(muted ? 0 : volume), TAG, "设置静音失败");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ctx.muted = muted;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", muted ? "已静音" : "已取消静音");
    return ESP_OK;
}

bool audio_service_is_muted(void)
{
    if (s_lock == NULL)
    {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool muted = s_ctx.muted;
    xSemaphoreGive(s_lock);
    return muted;
}

int audio_service_get_volume(void)
{
    if (s_lock == NULL)
    {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int volume = s_ctx.volume;
    xSemaphoreGive(s_lock);
    return volume;
}

bool audio_service_is_running(void)
{
    if (s_lock == NULL)
    {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool running = s_ctx.input_running || s_ctx.output_running;
    xSemaphoreGive(s_lock);
    return running;
}

bool audio_service_is_initialized(void)
{
    if (s_lock == NULL)
    {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool initialized = s_ctx.initialized;
    xSemaphoreGive(s_lock);
    return initialized;
}

uint32_t audio_service_get_sample_rate_hz(void)
{
    return audio_service_is_initialized() ? device_audio_get_sample_rate_hz() : 0U;
}
