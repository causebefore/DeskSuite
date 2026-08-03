#include "audio_service.h"
#include "audio_service_internal.hpp"

#include <assert.h>
#include <new>
#include <string.h>

#include "device_audio.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_types.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "audio_service";

ESP_EVENT_DEFINE_BASE(AUDIO_SERVICE_EVENT);

static AudioServiceRuntime *s_runtime;

AudioServiceRuntime::~AudioServiceRuntime() noexcept
{
    assert(lock == nullptr);
    assert(task_events == nullptr);
    assert(task == nullptr);
    assert(pcm_stream == nullptr);
    assert(pcm_stream_storage == nullptr);
    assert(mp3_decoder == nullptr);
    assert(channel_converter == nullptr);
    assert(rate_converter == nullptr);
    assert(encoded_buffer == nullptr);
    assert(decoded_buffer == nullptr);
    assert(channel_buffer == nullptr);
    assert(rate_buffer == nullptr);
    assert(pcm_input_buffer == nullptr);
    assert(!output_active);
    assert(!decoder_registered);
}

static TickType_t timeout_ticks(uint32_t timeout_ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
}

static void delete_fixed_resources(AudioServiceRuntime *runtime)
{
    if (runtime == nullptr)
    {
        return;
    }
    if (runtime->decoder_registered)
    {
        esp_audio_dec_unregister_default();
        runtime->decoder_registered = false;
    }
    runtime->pcm_stream = nullptr;
    heap_caps_free(runtime->pcm_stream_storage);
    runtime->pcm_stream_storage = nullptr;
    if (runtime->task_events != nullptr)
    {
        vEventGroupDelete(runtime->task_events);
        runtime->task_events = nullptr;
    }
    if (runtime->lock != nullptr)
    {
        vSemaphoreDelete(runtime->lock);
        runtime->lock = nullptr;
    }
}

esp_err_t audio_service_init(void)
{
    if (s_runtime != nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(device_audio_get_sample_rate_hz() > 0U,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "Device 音频能力尚未初始化");

    AudioServiceRuntime *runtime = new (std::nothrow) AudioServiceRuntime();
    ESP_RETURN_ON_FALSE(runtime != nullptr, ESP_ERR_NO_MEM, TAG, "创建 Audio Service Runtime 失败");
    s_runtime     = runtime;

    runtime->lock = xSemaphoreCreateMutex();
    if (runtime->lock == nullptr)
    {
        delete_fixed_resources(runtime);
        s_runtime = nullptr;
        delete runtime;
        return ESP_ERR_NO_MEM;
    }
    runtime->task_events = xEventGroupCreate();
    if (runtime->task_events == nullptr)
    {
        delete_fixed_resources(runtime);
        s_runtime = nullptr;
        delete runtime;
        return ESP_ERR_NO_MEM;
    }
    runtime->pcm_stream_storage = static_cast<uint8_t *>(
        heap_caps_malloc(CONFIG_DESKMATE_AUDIO_PCM_STREAM_BYTES + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (runtime->pcm_stream_storage == nullptr)
    {
        delete_fixed_resources(runtime);
        s_runtime = nullptr;
        delete runtime;
        return ESP_ERR_NO_MEM;
    }
    runtime->pcm_stream = xStreamBufferCreateStatic(CONFIG_DESKMATE_AUDIO_PCM_STREAM_BYTES,
                                                    1,
                                                    runtime->pcm_stream_storage,
                                                    &runtime->pcm_stream_struct);
    if (runtime->pcm_stream == nullptr)
    {
        delete_fixed_resources(runtime);
        s_runtime = nullptr;
        delete runtime;
        return ESP_ERR_NO_MEM;
    }

    const esp_audio_err_t decoder_error = esp_audio_dec_register_default();
    if (decoder_error != ESP_AUDIO_ERR_OK)
    {
        ESP_LOGE(TAG, "注册 MP3 解码器失败: %d", (int) decoder_error);
        delete_fixed_resources(runtime);
        s_runtime = nullptr;
        delete runtime;
        return ESP_FAIL;
    }
    runtime->decoder_registered = true;
    runtime->state              = AUDIO_SERVICE_STATE_STOPPED;
    runtime->last_error         = ESP_OK;
    ESP_LOGI(TAG,
             "Audio Service Runtime 初始化完成，PCM 缓冲=%u 字节",
             (unsigned) CONFIG_DESKMATE_AUDIO_PCM_STREAM_BYTES);
    return ESP_OK;
}

esp_err_t audio_service_start(void)
{
    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (runtime->state == AUDIO_SERVICE_STATE_RUNNING)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_OK;
    }
    if (runtime->state != AUDIO_SERVICE_STATE_STOPPED || runtime->task != nullptr)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_INVALID_STATE;
    }
    runtime->stop_requested        = false;
    runtime->pcm_open_requested    = false;
    runtime->pcm_close_requested   = false;
    runtime->file_cancel_requested = false;
    runtime->pcm_writers_active    = 0U;
    runtime->task_parked           = false;
    runtime->state                 = AUDIO_SERVICE_STATE_RUNNING;
    runtime->last_error            = ESP_OK;
    xStreamBufferReset(runtime->pcm_stream);
    xEventGroupClearBits(runtime->task_events, AUDIO_SERVICE_TASK_CONTROL_DONE | AUDIO_SERVICE_TASK_EXITED);
    const esp_err_t task_error = audio_service_playback_task_start(runtime);
    if (task_error != ESP_OK)
    {
        runtime->state       = AUDIO_SERVICE_STATE_STOPPED;
        runtime->task_parked = true;
        runtime->last_error  = task_error;
        xSemaphoreGive(runtime->lock);
        return task_error;
    }
    xSemaphoreGive(runtime->lock);
    ESP_LOGI(TAG, "Audio Service 已启动，播放 Task 等待事务");
    return ESP_OK;
}

esp_err_t audio_service_stop(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0U, ESP_ERR_INVALID_ARG, TAG, "Audio Service 停止超时无效");
    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if ((runtime->state == AUDIO_SERVICE_STATE_STOPPED || runtime->state == AUDIO_SERVICE_STATE_CLEANUP_FAILED)
        && runtime->task == nullptr && !runtime->output_active)
    {
        runtime->state      = AUDIO_SERVICE_STATE_STOPPED;
        runtime->last_error = ESP_OK;
        xSemaphoreGive(runtime->lock);
        return ESP_OK;
    }
    if (runtime->state != AUDIO_SERVICE_STATE_RUNNING && runtime->state != AUDIO_SERVICE_STATE_CLEANUP_FAILED)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_INVALID_STATE;
    }
    runtime->state          = AUDIO_SERVICE_STATE_STOPPING;
    runtime->stop_requested = true;
    xEventGroupClearBits(runtime->task_events, AUDIO_SERVICE_TASK_EXITED);
    audio_service_playback_task_notify(runtime);
    xSemaphoreGive(runtime->lock);

    const EventBits_t bits = xEventGroupWaitBits(runtime->task_events,
                                                 AUDIO_SERVICE_TASK_EXITED,
                                                 pdFALSE,
                                                 pdTRUE,
                                                 timeout_ticks(timeout_ms));
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if ((bits & AUDIO_SERVICE_TASK_EXITED) == 0 || runtime->task != nullptr || runtime->output_active)
    {
        runtime->state      = AUDIO_SERVICE_STATE_CLEANUP_FAILED;
        runtime->last_error = ESP_ERR_TIMEOUT;
        xSemaphoreGive(runtime->lock);
        ESP_LOGE(TAG, "等待播放 Task 退出超时: %u ms", (unsigned) timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    runtime->state      = AUDIO_SERVICE_STATE_STOPPED;
    runtime->last_error = ESP_OK;
    xSemaphoreGive(runtime->lock);
    ESP_LOGI(TAG, "Audio Service 已停止");
    return ESP_OK;
}

esp_err_t audio_service_deinit(void)
{
    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    const bool can_deinit = runtime->state == AUDIO_SERVICE_STATE_STOPPED && runtime->task == nullptr
                            && !runtime->output_active && runtime->active_stream_id == 0U
                            && runtime->active_request_id == 0U && runtime->pending_request_id == 0U
                            && runtime->pcm_writers_active == 0U;
    xSemaphoreGive(runtime->lock);
    if (!can_deinit)
    {
        return ESP_ERR_INVALID_STATE;
    }

    delete_fixed_resources(runtime);
    s_runtime = nullptr;
    delete runtime;
    ESP_LOGI(TAG, "Audio Service 已反初始化");
    return ESP_OK;
}

esp_err_t audio_service_get_status_copy(audio_service_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != nullptr, ESP_ERR_INVALID_ARG, TAG, "音频状态输出为空");
    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    *out_status = {
        .state              = runtime->state,
        .playback_state     = runtime->playback_state,
        .output_active      = runtime->output_active,
        .task_created       = runtime->task != nullptr,
        .task_parked        = runtime->task_parked,
        .active_request_id  = runtime->active_request_id,
        .pending_request_id = runtime->pending_request_id,
        .active_stream_id   = runtime->active_stream_id,
        .last_error         = runtime->last_error,
    };
    xSemaphoreGive(runtime->lock);
    return ESP_OK;
}

esp_err_t audio_service_request_play_mp3_file_copy(const char *path, uint64_t request_id)
{
    ESP_RETURN_ON_FALSE(path != nullptr && path[0] == '/' && request_id != 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "MP3 路径或请求 ID 无效");
    const size_t path_len = strnlen(path, AUDIO_SERVICE_FILE_PATH_MAX);
    ESP_RETURN_ON_FALSE(path_len > 0U && path_len < AUDIO_SERVICE_FILE_PATH_MAX,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "MP3 路径过长");

    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (runtime->state != AUDIO_SERVICE_STATE_RUNNING)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (runtime->active_request_id == request_id || runtime->pending_request_id == request_id
        || runtime->last_finished_request_id == request_id)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_OK;
    }
    if (runtime->active_request_id != 0U || runtime->pending_request_id != 0U)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(runtime->pending_file_path, path, path_len + 1U);
    runtime->pending_request_id = request_id;
    audio_service_playback_task_notify(runtime);
    xSemaphoreGive(runtime->lock);
    return ESP_OK;
}

esp_err_t audio_service_request_cancel_file_playback(uint64_t request_id)
{
    ESP_RETURN_ON_FALSE(request_id != 0U, ESP_ERR_INVALID_ARG, TAG, "取消播放请求 ID 无效");
    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (runtime->state != AUDIO_SERVICE_STATE_RUNNING && runtime->state != AUDIO_SERVICE_STATE_STOPPING)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (runtime->active_request_id != request_id && runtime->pending_request_id != request_id)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_NOT_FOUND;
    }
    runtime->file_cancel_requested  = true;
    runtime->file_cancel_request_id = request_id;
    audio_service_playback_task_notify(runtime);
    xSemaphoreGive(runtime->lock);
    return ESP_OK;
}

esp_err_t audio_service_open_pcm_stream(const audio_service_pcm_stream_config_t *config, uint32_t timeout_ms,
                                        uint64_t *out_stream_id)
{
    ESP_RETURN_ON_FALSE(config != nullptr && out_stream_id != nullptr && timeout_ms > 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "PCM 流参数为空或超时无效");
    *out_stream_id = 0U;
    ESP_RETURN_ON_FALSE(config->sample_rate_hz > 0U && (config->channel_count == 1U || config->channel_count == 2U),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "PCM 流格式无效");

    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (runtime->state != AUDIO_SERVICE_STATE_RUNNING || runtime->active_stream_id != 0U || runtime->pcm_open_requested)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t stream_id = runtime->next_stream_id++;
    if (runtime->next_stream_id == 0U)
    {
        runtime->next_stream_id = 1U;
    }
    runtime->requested_stream_id  = stream_id;
    runtime->requested_pcm_config = *config;
    runtime->pcm_open_requested   = true;
    runtime->control_result       = ESP_ERR_TIMEOUT;
    xEventGroupClearBits(runtime->task_events, AUDIO_SERVICE_TASK_CONTROL_DONE);
    audio_service_playback_task_notify(runtime);
    xSemaphoreGive(runtime->lock);

    const EventBits_t bits = xEventGroupWaitBits(runtime->task_events,
                                                 AUDIO_SERVICE_TASK_CONTROL_DONE,
                                                 pdTRUE,
                                                 pdTRUE,
                                                 timeout_ticks(timeout_ms));
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if ((bits & AUDIO_SERVICE_TASK_CONTROL_DONE) == 0)
    {
        if (runtime->active_stream_id == stream_id)
        {
            *out_stream_id = stream_id;
            xSemaphoreGive(runtime->lock);
            return ESP_OK;
        }
        if (runtime->pcm_open_requested && runtime->requested_stream_id == stream_id)
        {
            runtime->pcm_open_requested  = false;
            runtime->requested_stream_id = 0U;
        }
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = runtime->control_result;
    if (result == ESP_OK && runtime->active_stream_id == stream_id)
    {
        *out_stream_id = stream_id;
    }
    xSemaphoreGive(runtime->lock);
    return result;
}

esp_err_t audio_service_write_pcm_stream_borrow(uint64_t stream_id, const int16_t *samples, size_t sample_count,
                                                uint32_t timeout_ms, size_t *out_written)
{
    ESP_RETURN_ON_FALSE(samples != nullptr && out_written != nullptr && stream_id != 0U && sample_count > 0U
                            && timeout_ms > 0U && sample_count <= SIZE_MAX / sizeof(int16_t),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "PCM 写入参数无效");
    *out_written                 = 0U;
    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    const bool    writable = runtime->state == AUDIO_SERVICE_STATE_RUNNING && runtime->active_stream_id == stream_id
                             && !runtime->pcm_close_requested && runtime->pcm_writers_active == 0U;
    const uint8_t channel_count = runtime->active_pcm_config.channel_count;
    if (!writable)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count % channel_count != 0U)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_INVALID_SIZE;
    }
    runtime->pcm_writers_active++;
    xSemaphoreGive(runtime->lock);

    const size_t requested_bytes = sample_count * sizeof(int16_t);
    const size_t written_bytes =
        xStreamBufferSend(runtime->pcm_stream, samples, requested_bytes, timeout_ticks(timeout_ms));
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->pcm_writers_active--;
    audio_service_playback_task_notify(runtime);
    xSemaphoreGive(runtime->lock);
    *out_written = written_bytes / sizeof(int16_t);
    return written_bytes == requested_bytes ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_service_close_pcm_stream(uint64_t stream_id, bool discard, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(stream_id != 0U && timeout_ms > 0U, ESP_ERR_INVALID_ARG, TAG, "PCM 关闭请求 ID 或超时无效");
    AudioServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->lock == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (runtime->active_stream_id != stream_id)
    {
        if (runtime->last_closed_stream_id == stream_id)
        {
            const esp_err_t result = runtime->last_closed_stream_result;
            xSemaphoreGive(runtime->lock);
            return result;
        }
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_NOT_FOUND;
    }
    xEventGroupClearBits(runtime->task_events, AUDIO_SERVICE_TASK_CONTROL_DONE);
    if (!runtime->pcm_close_requested)
    {
        runtime->pcm_close_requested   = true;
        runtime->pcm_discard_requested = discard;
        runtime->control_result        = ESP_ERR_TIMEOUT;
    }
    else if (discard)
    {
        runtime->pcm_discard_requested = true;
    }
    audio_service_playback_task_notify(runtime);
    xSemaphoreGive(runtime->lock);

    const EventBits_t bits = xEventGroupWaitBits(runtime->task_events,
                                                 AUDIO_SERVICE_TASK_CONTROL_DONE,
                                                 pdTRUE,
                                                 pdTRUE,
                                                 timeout_ticks(timeout_ms));
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if ((bits & AUDIO_SERVICE_TASK_CONTROL_DONE) == 0 && runtime->active_stream_id == stream_id)
    {
        xSemaphoreGive(runtime->lock);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = runtime->active_stream_id == 0U && runtime->last_closed_stream_id == stream_id
                                 ? runtime->last_closed_stream_result
                                 : runtime->control_result;
    xSemaphoreGive(runtime->lock);
    return result;
}
