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
    audio_service_state_t state;
    bool                  input_running;  /* 麦克风链路是否在跑 */
    bool                  output_running; /* 扬声器链路是否在跑 */
    bool                  muted;          /* 是否静音 */
    int                   volume;         /* 真实音量值，静音时保留 */
    esp_err_t             last_error;
} audio_service_ctx_t;

static audio_service_ctx_t s_ctx;
static SemaphoreHandle_t   s_lock;

esp_err_t audio_service_init(void)
{
    if (s_lock != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(device_audio_get_sample_rate_hz() > 0U,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "Device 音频能力尚未初始化");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建音频互斥锁失败");

    s_ctx.state      = AUDIO_SERVICE_STATE_STOPPED;
    s_ctx.volume     = CONFIG_DESKMATE_AUDIO_DEFAULT_VOLUME;
    s_ctx.last_error = ESP_OK;

    ESP_LOGI(TAG, "音频服务初始化完成");
    return ESP_OK;
}

esp_err_t audio_service_start(void)
{
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ctx.state == AUDIO_SERVICE_STATE_RUNNING)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_ctx.state != AUDIO_SERVICE_STATE_STOPPED)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.state      = AUDIO_SERVICE_STATE_RUNNING;
    s_ctx.last_error = ESP_OK;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "音频 Service 已启动，输入输出保持关闭");
    return ESP_OK;
}

esp_err_t audio_service_deinit(void)
{
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ctx.state != AUDIO_SERVICE_STATE_STOPPED)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lock);
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_ctx  = (audio_service_ctx_t) { 0 };
    ESP_LOGI(TAG, "音频服务已反初始化");
    return ESP_OK;
}

esp_err_t audio_service_enable_input(bool enable)
{
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (enable && s_ctx.state != AUDIO_SERVICE_STATE_RUNNING)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.input_running == enable)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    const esp_err_t error = device_audio_enable_input(enable);
    if (error != ESP_OK)
    {
        s_ctx.last_error = error;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "%s: %s", enable ? "使能录音失败" : "关闭录音失败", esp_err_to_name(error));
        return error;
    }
    s_ctx.input_running = enable;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_service_enable_output(bool enable)
{
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (enable && s_ctx.state != AUDIO_SERVICE_STATE_RUNNING)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.output_running == enable)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    esp_err_t error = device_audio_enable_output(enable);
    if (error != ESP_OK)
    {
        s_ctx.last_error = error;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "%s: %s", enable ? "使能播放失败" : "关闭播放失败", esp_err_to_name(error));
        return error;
    }
    if (enable)
    {
        /* 恢复音量（考虑静音状态） */
        const esp_err_t volume_error = device_audio_set_output_volume(s_ctx.muted ? 0 : s_ctx.volume);
        if (volume_error != ESP_OK)
        {
            const esp_err_t rollback_error = device_audio_enable_output(false);
            if (rollback_error != ESP_OK)
            {
                s_ctx.output_running = true;
                s_ctx.state          = AUDIO_SERVICE_STATE_CLEANUP_FAILED;
                s_ctx.last_error     = rollback_error;
                ESP_LOGE(TAG,
                         "播放音量设置失败且输出回滚失败: primary=%s recovery=%s",
                         esp_err_to_name(volume_error),
                         esp_err_to_name(rollback_error));
            }
            else
            {
                s_ctx.last_error = volume_error;
            }
            xSemaphoreGive(s_lock);
            return volume_error;
        }
    }
    s_ctx.output_running = enable;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_service_stop(void)
{
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ctx.state == AUDIO_SERVICE_STATE_STOPPED)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_ctx.state != AUDIO_SERVICE_STATE_RUNNING && s_ctx.state != AUDIO_SERVICE_STATE_CLEANUP_FAILED)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.state = AUDIO_SERVICE_STATE_STOPPING;
    esp_err_t first_error = ESP_OK;
    if (s_ctx.input_running)
    {
        const esp_err_t error = device_audio_enable_input(false);
        if (error == ESP_OK)
        {
            s_ctx.input_running = false;
        }
        else
        {
            first_error = error;
        }
    }
    if (s_ctx.output_running)
    {
        const esp_err_t error = device_audio_enable_output(false);
        if (error == ESP_OK)
        {
            s_ctx.output_running = false;
        }
        else if (first_error == ESP_OK)
        {
            first_error = error;
        }
    }

    if (first_error == ESP_OK && !s_ctx.input_running && !s_ctx.output_running)
    {
        s_ctx.state      = AUDIO_SERVICE_STATE_STOPPED;
        s_ctx.last_error = ESP_OK;
    }
    else
    {
        s_ctx.state      = AUDIO_SERVICE_STATE_CLEANUP_FAILED;
        s_ctx.last_error = first_error != ESP_OK ? first_error : ESP_FAIL;
    }
    const audio_service_state_t state = s_ctx.state;
    xSemaphoreGive(s_lock);
    if (state == AUDIO_SERVICE_STATE_STOPPED)
    {
        ESP_LOGI(TAG, "音频链路已停止");
    }
    else
    {
        ESP_LOGE(TAG, "音频链路停止不完整: %s", esp_err_to_name(first_error));
    }
    return first_error;
}

esp_err_t audio_service_get_status_copy(audio_service_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "音频状态输出为空");
    if (s_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out_status = (audio_service_status_t) {
        .state         = s_ctx.state,
        .input_active  = s_ctx.input_running,
        .output_active = s_ctx.output_running,
        .last_error    = s_ctx.last_error,
    };
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_service_write(const int16_t *data, size_t sample_count, size_t *out_written)
{
    ESP_RETURN_ON_FALSE(out_written != NULL, ESP_ERR_INVALID_ARG, TAG, "播放写入计数输出为空");
    *out_written = 0U;
    if (s_lock == NULL)
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
    if (s_lock == NULL)
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
    if (s_lock == NULL)
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
    if (s_lock == NULL)
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
    const bool initialized = s_ctx.state != AUDIO_SERVICE_STATE_UNINITIALIZED;
    xSemaphoreGive(s_lock);
    return initialized;
}

uint32_t audio_service_get_sample_rate_hz(void)
{
    return audio_service_is_initialized() ? device_audio_get_sample_rate_hz() : 0U;
}
