#include "audio_service_internal.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "device_audio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

namespace {

constexpr size_t      kEncodedBufferBytes      = 4096U;
constexpr size_t      kInitialDecodedBytes     = 8192U;
constexpr size_t      kMaximumDecodedBytes     = 65536U;
constexpr size_t      kPcmInputCapacitySamples = 2048U;
constexpr size_t      kOutputWriteChunkSamples = 480U;
constexpr uint32_t    kPlaybackMaximumSeconds  = 30U;
constexpr uint32_t    kPlaybackTaskStackBytes  = 20U * 1024U;
constexpr UBaseType_t kPlaybackTaskPriority    = 3U;

const char *TAG                                = "audio_playback";

struct FilePlaybackOutcome
{
    audio_service_file_playback_result_state_t state{ AUDIO_SERVICE_FILE_PLAYBACK_RESULT_FAILED };
    esp_err_t                                  error{ ESP_FAIL };
};

void set_last_error(AudioServiceRuntime *runtime, esp_err_t error)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->last_error = error;
    xSemaphoreGive(runtime->lock);
}

esp_err_t set_output_enabled(AudioServiceRuntime *runtime, bool enabled)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    const bool already_enabled = runtime->output_active;
    xSemaphoreGive(runtime->lock);
    if (already_enabled == enabled)
    {
        return ESP_OK;
    }

    esp_err_t error = device_audio_enable_output(enabled);
    if (error == ESP_OK && enabled)
    {
        error = device_audio_set_output_volume(CONFIG_DESKMATE_AUDIO_DEFAULT_VOLUME);
        if (error != ESP_OK)
        {
            const esp_err_t rollback_error = device_audio_enable_output(false);
            xSemaphoreTake(runtime->lock, portMAX_DELAY);
            runtime->output_active = rollback_error != ESP_OK;
            runtime->last_error    = rollback_error != ESP_OK ? rollback_error : error;
            xSemaphoreGive(runtime->lock);
            ESP_LOGE(TAG,
                     "设置播放音量失败%s: primary=%s recovery=%s",
                     rollback_error == ESP_OK ? "" : "且输出回滚失败",
                     esp_err_to_name(error),
                     esp_err_to_name(rollback_error));
            return error;
        }
    }

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (error == ESP_OK)
    {
        runtime->output_active = enabled;
    }
    runtime->last_error = error;
    xSemaphoreGive(runtime->lock);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "%s音频输出失败: %s", enabled ? "启用" : "关闭", esp_err_to_name(error));
    }
    return error;
}

void close_decoder(AudioServiceRuntime *runtime)
{
    if (runtime->mp3_decoder != nullptr)
    {
        esp_audio_simple_dec_close(runtime->mp3_decoder);
        runtime->mp3_decoder = nullptr;
    }
}

void close_converters(AudioServiceRuntime *runtime)
{
    if (runtime->rate_converter != nullptr)
    {
        esp_ae_rate_cvt_close(runtime->rate_converter);
        runtime->rate_converter = nullptr;
    }
    if (runtime->channel_converter != nullptr)
    {
        esp_ae_ch_cvt_close(runtime->channel_converter);
        runtime->channel_converter = nullptr;
    }
}

void free_work_buffers(AudioServiceRuntime *runtime)
{
    heap_caps_free(runtime->encoded_buffer);
    runtime->encoded_buffer   = nullptr;
    runtime->encoded_capacity = 0U;
    heap_caps_free(runtime->decoded_buffer);
    runtime->decoded_buffer   = nullptr;
    runtime->decoded_capacity = 0U;
    heap_caps_free(runtime->channel_buffer);
    runtime->channel_buffer           = nullptr;
    runtime->channel_capacity_samples = 0U;
    heap_caps_free(runtime->rate_buffer);
    runtime->rate_buffer           = nullptr;
    runtime->rate_capacity_samples = 0U;
    heap_caps_free(runtime->pcm_input_buffer);
    runtime->pcm_input_buffer           = nullptr;
    runtime->pcm_input_capacity_samples = 0U;
}

esp_err_t reserve_bytes(uint8_t **buffer, size_t *capacity, size_t required)
{
    if (*capacity >= required)
    {
        return ESP_OK;
    }
    void *next = heap_caps_realloc(*buffer, required, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    *buffer   = static_cast<uint8_t *>(next);
    *capacity = required;
    return ESP_OK;
}

esp_err_t reserve_samples(int16_t **buffer, size_t *capacity, size_t required)
{
    if (*capacity >= required)
    {
        return ESP_OK;
    }
    if (required > SIZE_MAX / sizeof(int16_t))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    void *next = heap_caps_realloc(*buffer, required * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    *buffer   = static_cast<int16_t *>(next);
    *capacity = required;
    return ESP_OK;
}

esp_err_t open_converters(AudioServiceRuntime *runtime, uint32_t sample_rate_hz, uint8_t channel_count)
{
    close_converters(runtime);
    if (sample_rate_hz == 0U || (channel_count != 1U && channel_count != 2U))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (channel_count == 2U)
    {
        esp_ae_ch_cvt_cfg_t channel_config{};
        channel_config.sample_rate       = sample_rate_hz;
        channel_config.bits_per_sample   = 16U;
        channel_config.src_ch            = 2U;
        channel_config.dest_ch           = 1U;
        const esp_ae_err_t channel_error = esp_ae_ch_cvt_open(&channel_config, &runtime->channel_converter);
        if (channel_error != ESP_AE_ERR_OK)
        {
            ESP_LOGE(TAG, "创建双声道转单声道转换器失败: %d", static_cast<int>(channel_error));
            return channel_error == ESP_AE_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
    }

    const uint32_t output_rate_hz = device_audio_get_sample_rate_hz();
    if (sample_rate_hz != output_rate_hz)
    {
        esp_ae_rate_cvt_cfg_t rate_config{};
        rate_config.src_rate          = sample_rate_hz;
        rate_config.dest_rate         = output_rate_hz;
        rate_config.channel           = 1U;
        rate_config.bits_per_sample   = 16U;
        rate_config.complexity        = 2U;
        rate_config.perf_type         = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY;
        const esp_ae_err_t rate_error = esp_ae_rate_cvt_open(&rate_config, &runtime->rate_converter);
        if (rate_error != ESP_AE_ERR_OK)
        {
            ESP_LOGE(TAG, "创建输出重采样器失败: %d", static_cast<int>(rate_error));
            close_converters(runtime);
            return rate_error == ESP_AE_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
        }
    }
    return ESP_OK;
}

/** @brief 按硬件 20 ms 帧分块写出。 */
esp_err_t write_all_samples(const int16_t *samples, size_t sample_count)
{
    size_t offset = 0U;
    while (offset < sample_count)
    {
        const size_t    remaining   = sample_count - offset;
        const size_t    chunk_count = remaining < kOutputWriteChunkSamples ? remaining : kOutputWriteChunkSamples;
        size_t          written     = 0U;
        const esp_err_t error       = device_audio_write(samples + offset, chunk_count, &written);
        if (error != ESP_OK)
        {
            return error;
        }
        if (written == 0U || written > chunk_count)
        {
            return ESP_FAIL;
        }
        offset += written;
    }
    return ESP_OK;
}

esp_err_t convert_and_write(AudioServiceRuntime *runtime, const int16_t *samples, size_t sample_count,
                            uint8_t channel_count)
{
    if (samples == nullptr || sample_count == 0U || sample_count % channel_count != 0U)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t frame_count = sample_count / channel_count;
    if (frame_count > UINT32_MAX)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const int16_t *mono_samples = samples;
    if (channel_count == 2U)
    {
        esp_err_t error = reserve_samples(&runtime->channel_buffer, &runtime->channel_capacity_samples, frame_count);
        if (error != ESP_OK)
        {
            return error;
        }
        const esp_ae_err_t channel_error = esp_ae_ch_cvt_process(runtime->channel_converter,
                                                                 static_cast<uint32_t>(frame_count),
                                                                 const_cast<int16_t *>(samples),
                                                                 runtime->channel_buffer);
        if (channel_error != ESP_AE_ERR_OK)
        {
            return ESP_FAIL;
        }
        mono_samples = runtime->channel_buffer;
    }

    const int16_t *output_samples = mono_samples;
    size_t         output_count   = frame_count;
    if (runtime->rate_converter != nullptr)
    {
        uint32_t maximum_output_count = 0U;
        if (esp_ae_rate_cvt_get_max_out_sample_num(runtime->rate_converter,
                                                   static_cast<uint32_t>(frame_count),
                                                   &maximum_output_count)
            != ESP_AE_ERR_OK)
        {
            return ESP_FAIL;
        }
        esp_err_t error = reserve_samples(&runtime->rate_buffer, &runtime->rate_capacity_samples, maximum_output_count);
        if (error != ESP_OK)
        {
            return error;
        }
        uint32_t actual_output_count = maximum_output_count;
        if (esp_ae_rate_cvt_process(runtime->rate_converter,
                                    const_cast<int16_t *>(mono_samples),
                                    static_cast<uint32_t>(frame_count),
                                    runtime->rate_buffer,
                                    &actual_output_count)
            != ESP_AE_ERR_OK)
        {
            return ESP_FAIL;
        }
        output_samples = runtime->rate_buffer;
        output_count   = actual_output_count;
    }
    if (output_count == 0U)
    {
        return ESP_OK;
    }

    esp_err_t error = set_output_enabled(runtime, true);
    if (error != ESP_OK)
    {
        return error;
    }
    return write_all_samples(output_samples, output_count);
}

void publish_file_result(AudioServiceRuntime *runtime, uint64_t request_id,
                         audio_service_file_playback_result_state_t state, esp_err_t error)
{
    const audio_service_file_playback_result_t result{
        .request_id = request_id,
        .state      = state,
        .error      = error,
    };
    const esp_err_t post_error = esp_event_post(AUDIO_SERVICE_EVENT,
                                                AUDIO_SERVICE_EVENT_FILE_PLAYBACK_FINISHED,
                                                &result,
                                                sizeof(result),
                                                pdMS_TO_TICKS(1000U));
    if (post_error != ESP_OK)
    {
        set_last_error(runtime, post_error);
        ESP_LOGE(TAG,
                 "发布文件播放终态失败: request=%llu error=%s",
                 static_cast<unsigned long long>(request_id),
                 esp_err_to_name(post_error));
    }
}

bool file_should_cancel(AudioServiceRuntime *runtime, uint64_t request_id)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    const bool cancel = runtime->stop_requested || runtime->pcm_open_requested
                        || (runtime->file_cancel_requested && runtime->file_cancel_request_id == request_id);
    if (cancel)
    {
        runtime->playback_state = AUDIO_SERVICE_PLAYBACK_STATE_CANCELLING;
    }
    xSemaphoreGive(runtime->lock);
    return cancel;
}

FilePlaybackOutcome play_mp3_file(AudioServiceRuntime *runtime, const char *path, uint64_t request_id)
{
    FilePlaybackOutcome outcome{};
    outcome.error = set_output_enabled(runtime, false);
    if (outcome.error != ESP_OK)
    {
        return outcome;
    }
    FILE *file = fopen(path, "rb");
    if (file == nullptr)
    {
        outcome.error = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        ESP_LOGE(TAG, "打开 MP3 文件失败: path=%s errno=%d", path, errno);
        return outcome;
    }

    struct stat file_info;
    const int   descriptor = fileno(file);
    if (descriptor < 0 || fstat(descriptor, &file_info) != 0 || !S_ISREG(file_info.st_mode) || file_info.st_size <= 0)
    {
        fclose(file);
        outcome.error = ESP_ERR_INVALID_SIZE;
        ESP_LOGE(TAG, "MP3 文件为空或类型无效: path=%s", path);
        return outcome;
    }

    esp_err_t error = reserve_bytes(&runtime->encoded_buffer, &runtime->encoded_capacity, kEncodedBufferBytes);
    if (error == ESP_OK)
    {
        error = reserve_bytes(&runtime->decoded_buffer, &runtime->decoded_capacity, kInitialDecodedBytes);
    }
    if (error != ESP_OK)
    {
        fclose(file);
        outcome.error = error;
        return outcome;
    }

    esp_audio_simple_dec_cfg_t decoder_config{};
    decoder_config.dec_type          = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    decoder_config.use_frame_dec     = false;
    const esp_audio_err_t open_error = esp_audio_simple_dec_open(&decoder_config, &runtime->mp3_decoder);
    if (open_error != ESP_AUDIO_ERR_OK)
    {
        fclose(file);
        outcome.error = open_error == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
        ESP_LOGE(TAG, "创建 MP3 解码器失败: %d", static_cast<int>(open_error));
        return outcome;
    }

    bool           eof                    = false;
    bool           produced_audio         = false;
    bool           format_ready           = false;
    size_t         encoded_size           = 0U;
    uint64_t       file_bytes_read        = 0U;
    const uint64_t file_size_bytes        = static_cast<uint64_t>(file_info.st_size);
    uint8_t        channel_count          = 0U;
    uint64_t       decoded_frame_count    = 0U;
    uint64_t       maximum_decoded_frames = 0U;
    outcome.error                         = ESP_OK;

    for (;;)
    {
        if (file_should_cancel(runtime, request_id))
        {
            outcome.state = AUDIO_SERVICE_FILE_PLAYBACK_RESULT_CANCELLED;
            outcome.error = ESP_OK;
            break;
        }

        if (!eof && encoded_size < runtime->encoded_capacity)
        {
            const uint64_t remaining_file_bytes = file_size_bytes - file_bytes_read;
            const size_t   available_bytes      = runtime->encoded_capacity - encoded_size;
            const size_t   wanted_size =
                remaining_file_bytes < available_bytes ? static_cast<size_t>(remaining_file_bytes) : available_bytes;
            const size_t read_size = fread(runtime->encoded_buffer + encoded_size, 1U, wanted_size, file);
            encoded_size += read_size;
            file_bytes_read += read_size;
            if (read_size != wanted_size)
            {
                outcome.error = ESP_FAIL;
                ESP_LOGE(TAG, "读取 MP3 文件失败或播放期间文件被截断: path=%s errno=%d", path, errno);
                break;
            }
            eof = file_bytes_read == file_size_bytes;
        }

        if (eof && encoded_size == 0U)
        {
            outcome.state = produced_audio ? AUDIO_SERVICE_FILE_PLAYBACK_RESULT_COMPLETED
                                           : AUDIO_SERVICE_FILE_PLAYBACK_RESULT_FAILED;
            outcome.error = produced_audio ? ESP_OK : ESP_FAIL;
            break;
        }

        esp_audio_simple_dec_raw_t raw{};
        raw.buffer = runtime->encoded_buffer;
        raw.len    = static_cast<uint32_t>(encoded_size);
        raw.eos    = eof;
        esp_audio_simple_dec_out_t decoded{};
        decoded.buffer                     = runtime->decoded_buffer;
        decoded.len                        = static_cast<uint32_t>(runtime->decoded_capacity);

        const esp_audio_err_t decode_error = esp_audio_simple_dec_process(runtime->mp3_decoder, &raw, &decoded);
        if (decode_error == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
        {
            if (decoded.needed_size <= runtime->decoded_capacity || decoded.needed_size > kMaximumDecodedBytes)
            {
                outcome.error = ESP_ERR_INVALID_SIZE;
                ESP_LOGE(TAG, "MP3 解码输出超过上限: needed=%u", (unsigned) decoded.needed_size);
                break;
            }
            error = reserve_bytes(&runtime->decoded_buffer, &runtime->decoded_capacity, decoded.needed_size);
            if (error != ESP_OK)
            {
                outcome.error = error;
                break;
            }
            continue;
        }
        if (decode_error != ESP_AUDIO_ERR_OK || raw.consumed > encoded_size
            || decoded.decoded_size > runtime->decoded_capacity)
        {
            outcome.error = ESP_FAIL;
            ESP_LOGE(TAG, "MP3 数据解码失败: %d", static_cast<int>(decode_error));
            break;
        }

        if (raw.consumed > 0U)
        {
            encoded_size -= raw.consumed;
            if (encoded_size > 0U)
            {
                memmove(runtime->encoded_buffer, runtime->encoded_buffer + raw.consumed, encoded_size);
            }
        }

        if (decoded.decoded_size > 0U)
        {
            if (!format_ready)
            {
                esp_audio_simple_dec_info_t info{};
                const esp_audio_err_t       info_error = esp_audio_simple_dec_get_info(runtime->mp3_decoder, &info);
                if (info_error != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16U
                    || (info.channel != 1U && info.channel != 2U) || info.sample_rate == 0U)
                {
                    outcome.error = ESP_ERR_NOT_SUPPORTED;
                    ESP_LOGE(TAG,
                             "MP3 格式不受支持: result=%d rate=%u bits=%u channels=%u",
                             static_cast<int>(info_error),
                             (unsigned) info.sample_rate,
                             (unsigned) info.bits_per_sample,
                             (unsigned) info.channel);
                    break;
                }
                error = open_converters(runtime, info.sample_rate, info.channel);
                if (error != ESP_OK)
                {
                    outcome.error = error;
                    break;
                }
                channel_count          = info.channel;
                maximum_decoded_frames = static_cast<uint64_t>(info.sample_rate) * kPlaybackMaximumSeconds;
                format_ready           = true;
                ESP_LOGI(TAG,
                         "开始 MP3 播放: request=%llu rate=%u channels=%u",
                         static_cast<unsigned long long>(request_id),
                         (unsigned) info.sample_rate,
                         (unsigned) info.channel);
            }

            const size_t frame_bytes = sizeof(int16_t) * channel_count;
            if (decoded.decoded_size % frame_bytes != 0U)
            {
                outcome.error = ESP_ERR_INVALID_SIZE;
                break;
            }
            const uint64_t decoded_frames   = decoded.decoded_size / frame_bytes;
            const uint64_t remaining_frames = maximum_decoded_frames - decoded_frame_count;
            const uint64_t frames_to_play   = decoded_frames < remaining_frames ? decoded_frames : remaining_frames;
            if (frames_to_play > 0U)
            {
                error = convert_and_write(runtime,
                                          reinterpret_cast<const int16_t *>(decoded.buffer),
                                          static_cast<size_t>(frames_to_play) * channel_count,
                                          channel_count);
                if (error != ESP_OK)
                {
                    outcome.error = error;
                    ESP_LOGE(TAG, "写入 MP3 PCM 失败: %s", esp_err_to_name(error));
                    break;
                }
                decoded_frame_count += frames_to_play;
                produced_audio = true;
            }
            if (decoded_frame_count >= maximum_decoded_frames)
            {
                outcome.state = AUDIO_SERVICE_FILE_PLAYBACK_RESULT_COMPLETED;
                outcome.error = ESP_OK;
                ESP_LOGW(TAG, "MP3 播放达到 30 秒上限: request=%llu", static_cast<unsigned long long>(request_id));
                break;
            }
        }

        const bool made_progress = raw.consumed > 0U || decoded.decoded_size > 0U;
        if (eof && !made_progress)
        {
            outcome.error = ESP_FAIL;
            ESP_LOGE(TAG, "MP3 文件尾无法形成完整音频帧");
            break;
        }
        if (!eof && encoded_size == runtime->encoded_capacity && !made_progress)
        {
            outcome.error = ESP_ERR_INVALID_SIZE;
            ESP_LOGE(TAG, "MP3 解码器无法消费 4 KiB 输入块");
            break;
        }
    }

    close_decoder(runtime);
    close_converters(runtime);
    fclose(file);
    const esp_err_t disable_error = set_output_enabled(runtime, false);
    if (disable_error != ESP_OK)
    {
        outcome.state = AUDIO_SERVICE_FILE_PLAYBACK_RESULT_FAILED;
        outcome.error = disable_error;
    }
    if (outcome.state == AUDIO_SERVICE_FILE_PLAYBACK_RESULT_FAILED && outcome.error == ESP_OK)
    {
        outcome.error = ESP_FAIL;
    }
    return outcome;
}

uint64_t take_cancelled_pending_request(AudioServiceRuntime *runtime)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    uint64_t request_id = 0U;
    if (runtime->file_cancel_requested && runtime->pending_request_id != 0U
        && runtime->file_cancel_request_id == runtime->pending_request_id)
    {
        request_id                        = runtime->pending_request_id;
        runtime->pending_request_id       = 0U;
        runtime->last_finished_request_id = request_id;
        runtime->pending_file_path[0]     = '\0';
        runtime->file_cancel_requested    = false;
        runtime->file_cancel_request_id   = 0U;
        if (runtime->active_stream_id == 0U && runtime->active_request_id == 0U)
        {
            runtime->playback_state = AUDIO_SERVICE_PLAYBACK_STATE_IDLE;
        }
    }
    xSemaphoreGive(runtime->lock);
    return request_id;
}

void signal_control_result(AudioServiceRuntime *runtime, esp_err_t result)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->control_result = result;
    runtime->last_error     = result;
    xSemaphoreGive(runtime->lock);
    xEventGroupSetBits(runtime->task_events, AUDIO_SERVICE_TASK_CONTROL_DONE);
}

void run_pcm_stream(AudioServiceRuntime *runtime)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    const uint64_t                          stream_id = runtime->requested_stream_id;
    const audio_service_pcm_stream_config_t config    = runtime->requested_pcm_config;
    xSemaphoreGive(runtime->lock);

    esp_err_t error = set_output_enabled(runtime, false);
    if (error == ESP_OK)
    {
        error = open_converters(runtime, config.sample_rate_hz, config.channel_count);
    }
    if (error == ESP_OK)
    {
        error =
            reserve_samples(&runtime->pcm_input_buffer, &runtime->pcm_input_capacity_samples, kPcmInputCapacitySamples);
    }

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    const bool request_matches = runtime->pcm_open_requested && runtime->requested_stream_id == stream_id;
    const bool can_activate    = request_matches && runtime->state == AUDIO_SERVICE_STATE_RUNNING;
    if (error != ESP_OK || !can_activate)
    {
        if (request_matches)
        {
            runtime->pcm_open_requested  = false;
            runtime->requested_stream_id = 0U;
            runtime->control_result      = error != ESP_OK ? error : ESP_ERR_INVALID_STATE;
            runtime->last_error          = runtime->control_result;
        }
        xSemaphoreGive(runtime->lock);
        close_converters(runtime);
        if (request_matches)
        {
            xEventGroupSetBits(runtime->task_events, AUDIO_SERVICE_TASK_CONTROL_DONE);
        }
        return;
    }
    xStreamBufferReset(runtime->pcm_stream);
    runtime->active_pcm_config     = config;
    runtime->active_stream_id      = stream_id;
    runtime->requested_stream_id   = 0U;
    runtime->pcm_open_requested    = false;
    runtime->pcm_close_requested   = false;
    runtime->pcm_discard_requested = false;
    runtime->playback_state        = AUDIO_SERVICE_PLAYBACK_STATE_PCM_STREAM;
    runtime->control_result        = ESP_OK;
    runtime->last_error            = ESP_OK;
    xSemaphoreGive(runtime->lock);
    xEventGroupSetBits(runtime->task_events, AUDIO_SERVICE_TASK_CONTROL_DONE);

    esp_err_t transaction_error = ESP_OK;
    for (;;)
    {
        const uint64_t cancelled_request_id = take_cancelled_pending_request(runtime);
        if (cancelled_request_id != 0U)
        {
            publish_file_result(runtime, cancelled_request_id, AUDIO_SERVICE_FILE_PLAYBACK_RESULT_CANCELLED, ESP_OK);
        }

        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        const bool stop_requested    = runtime->stop_requested;
        bool       close_requested   = runtime->pcm_close_requested;
        bool       discard_requested = runtime->pcm_discard_requested;
        if (stop_requested)
        {
            close_requested                = true;
            discard_requested              = true;
            runtime->pcm_close_requested   = true;
            runtime->pcm_discard_requested = true;
        }
        if (close_requested)
        {
            runtime->playback_state =
                discard_requested ? AUDIO_SERVICE_PLAYBACK_STATE_CANCELLING : AUDIO_SERVICE_PLAYBACK_STATE_DRAINING;
        }
        const uint32_t writers_active = runtime->pcm_writers_active;
        xSemaphoreGive(runtime->lock);

        if (discard_requested)
        {
            if (writers_active != 0U)
            {
                (void) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20U));
                continue;
            }
            xStreamBufferReset(runtime->pcm_stream);
        }
        if (close_requested && writers_active == 0U && xStreamBufferBytesAvailable(runtime->pcm_stream) == 0U)
        {
            error = set_output_enabled(runtime, false);
            if (error != ESP_OK)
            {
                if (transaction_error == ESP_OK)
                {
                    transaction_error = error;
                }
                signal_control_result(runtime, error);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
                continue;
            }

            close_converters(runtime);
            xSemaphoreTake(runtime->lock, portMAX_DELAY);
            runtime->active_stream_id          = 0U;
            runtime->active_pcm_config         = {};
            runtime->pcm_close_requested       = false;
            runtime->pcm_discard_requested     = false;
            runtime->last_closed_stream_id     = stream_id;
            runtime->last_closed_stream_result = transaction_error;
            runtime->control_result            = transaction_error;
            runtime->last_error                = transaction_error;
            runtime->playback_state = runtime->pending_request_id != 0U ? AUDIO_SERVICE_PLAYBACK_STATE_FILE_PENDING
                                                                        : AUDIO_SERVICE_PLAYBACK_STATE_IDLE;
            xSemaphoreGive(runtime->lock);
            xEventGroupSetBits(runtime->task_events, AUDIO_SERVICE_TASK_CONTROL_DONE);
            return;
        }

        const size_t received_bytes = xStreamBufferReceive(runtime->pcm_stream,
                                                           runtime->pcm_input_buffer,
                                                           runtime->pcm_input_capacity_samples * sizeof(int16_t),
                                                           pdMS_TO_TICKS(20U));
        if (received_bytes == 0U)
        {
            continue;
        }
        if (received_bytes % sizeof(int16_t) != 0U)
        {
            error = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            error = convert_and_write(runtime,
                                      runtime->pcm_input_buffer,
                                      received_bytes / sizeof(int16_t),
                                      config.channel_count);
        }
        if (error != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "PCM 流输出失败: stream=%llu error=%s",
                     static_cast<unsigned long long>(stream_id),
                     esp_err_to_name(error));
            xSemaphoreTake(runtime->lock, portMAX_DELAY);
            if (transaction_error == ESP_OK)
            {
                transaction_error = error;
            }
            runtime->last_error            = error;
            runtime->pcm_close_requested   = true;
            runtime->pcm_discard_requested = true;
            xSemaphoreGive(runtime->lock);
        }
    }
}

void finish_active_file(AudioServiceRuntime *runtime, uint64_t request_id, const FilePlaybackOutcome &outcome)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->active_request_id        = 0U;
    runtime->last_finished_request_id = request_id;
    runtime->pending_file_path[0]     = '\0';
    if (runtime->file_cancel_requested && runtime->file_cancel_request_id == request_id)
    {
        runtime->file_cancel_requested  = false;
        runtime->file_cancel_request_id = 0U;
    }
    runtime->last_error     = outcome.error;
    runtime->playback_state = runtime->pending_request_id != 0U ? AUDIO_SERVICE_PLAYBACK_STATE_FILE_PENDING
                                                                : AUDIO_SERVICE_PLAYBACK_STATE_IDLE;
    xSemaphoreGive(runtime->lock);
    publish_file_result(runtime, request_id, outcome.state, outcome.error);
}

void cancel_all_pending_files(AudioServiceRuntime *runtime)
{
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    const uint64_t request_id   = runtime->pending_request_id;
    runtime->pending_request_id = 0U;
    if (request_id != 0U)
    {
        runtime->last_finished_request_id = request_id;
    }
    runtime->pending_file_path[0]   = '\0';
    runtime->file_cancel_requested  = false;
    runtime->file_cancel_request_id = 0U;
    xSemaphoreGive(runtime->lock);
    if (request_id != 0U)
    {
        publish_file_result(runtime, request_id, AUDIO_SERVICE_FILE_PLAYBACK_RESULT_CANCELLED, ESP_OK);
    }
}

void playback_task(void *arg)
{
    auto *runtime = static_cast<AudioServiceRuntime *>(arg);
    for (;;)
    {
        const uint64_t cancelled_request_id = take_cancelled_pending_request(runtime);
        if (cancelled_request_id != 0U)
        {
            publish_file_result(runtime, cancelled_request_id, AUDIO_SERVICE_FILE_PLAYBACK_RESULT_CANCELLED, ESP_OK);
        }

        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        const bool     stop_requested = runtime->stop_requested;
        const bool     open_pcm       = runtime->pcm_open_requested;
        const uint64_t pending_file   = runtime->pending_request_id;
        xSemaphoreGive(runtime->lock);

        if (stop_requested)
        {
            cancel_all_pending_files(runtime);
            xStreamBufferReset(runtime->pcm_stream);
            if (set_output_enabled(runtime, false) != ESP_OK)
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
                continue;
            }
            break;
        }
        if (open_pcm)
        {
            run_pcm_stream(runtime);
            continue;
        }
        if (pending_file != 0U)
        {
            xSemaphoreTake(runtime->lock, portMAX_DELAY);
            if (runtime->pending_request_id != pending_file)
            {
                xSemaphoreGive(runtime->lock);
                continue;
            }
            runtime->active_request_id  = pending_file;
            runtime->pending_request_id = 0U;
            runtime->playback_state     = AUDIO_SERVICE_PLAYBACK_STATE_FILE;
            runtime->last_error         = ESP_OK;
            char path[AUDIO_SERVICE_FILE_PATH_MAX];
            memcpy(path, runtime->pending_file_path, sizeof(path));
            xSemaphoreGive(runtime->lock);

            const FilePlaybackOutcome outcome = play_mp3_file(runtime, path, pending_file);
            finish_active_file(runtime, pending_file, outcome);
            continue;
        }

        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        runtime->task_parked    = true;
        runtime->playback_state = AUDIO_SERVICE_PLAYBACK_STATE_IDLE;
        xSemaphoreGive(runtime->lock);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        runtime->task_parked = false;
        xSemaphoreGive(runtime->lock);
    }

    close_decoder(runtime);
    close_converters(runtime);
    free_work_buffers(runtime);
    const UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->task                  = nullptr;
    runtime->task_parked           = true;
    runtime->playback_state        = AUDIO_SERVICE_PLAYBACK_STATE_IDLE;
    runtime->active_stream_id      = 0U;
    runtime->active_request_id     = 0U;
    runtime->requested_stream_id   = 0U;
    runtime->pcm_open_requested    = false;
    runtime->pcm_close_requested   = false;
    runtime->pcm_discard_requested = false;
    runtime->pcm_writers_active    = 0U;
    xSemaphoreGive(runtime->lock);
    ESP_LOGI(TAG, "播放 Task 已退出，栈高水位=%u 字节", (unsigned) stack_high_water);
    xEventGroupSetBits(runtime->task_events, AUDIO_SERVICE_TASK_EXITED);
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

esp_err_t audio_service_playback_task_start(AudioServiceRuntime *runtime)
{
    if (runtime == nullptr || runtime->task != nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const BaseType_t result = xTaskCreateWithCaps(playback_task,
                                                  "audio_playback",
                                                  kPlaybackTaskStackBytes,
                                                  runtime,
                                                  kPlaybackTaskPriority,
                                                  &runtime->task,
                                                  MALLOC_CAP_SPIRAM);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void audio_service_playback_task_notify(AudioServiceRuntime *runtime)
{
    if (runtime != nullptr && runtime->task != nullptr)
    {
        xTaskNotifyGive(runtime->task);
    }
}
