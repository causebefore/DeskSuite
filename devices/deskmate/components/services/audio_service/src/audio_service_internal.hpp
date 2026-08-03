#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_service.h"
#include "esp_ae_ch_cvt.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define AUDIO_SERVICE_FILE_PATH_MAX     256U

#define AUDIO_SERVICE_TASK_CONTROL_DONE BIT0
#define AUDIO_SERVICE_TASK_EXITED       BIT1

/**
 * @brief Audio Service 的私有输出 Runtime
 *
 * 固定资源在 init/deinit 间存在；播放 Task 在 start/stop 间存在。析构函数只验证显式清理
 * 已完成，不执行阻塞或可能失败的关闭操作。
 */
class AudioServiceRuntime final {
  public:
    AudioServiceRuntime() noexcept = default;
    ~AudioServiceRuntime() noexcept;

    AudioServiceRuntime(const AudioServiceRuntime &)            = delete;
    AudioServiceRuntime &operator=(const AudioServiceRuntime &) = delete;

    SemaphoreHandle_t  lock                                     = nullptr;
    EventGroupHandle_t task_events                              = nullptr;
    TaskHandle_t       task                                     = nullptr;

    uint8_t             *pcm_stream_storage                     = nullptr;
    StaticStreamBuffer_t pcm_stream_struct{};
    StreamBufferHandle_t pcm_stream = nullptr;

    esp_audio_simple_dec_handle_t mp3_decoder{};
    esp_ae_ch_cvt_handle_t        channel_converter{};
    esp_ae_rate_cvt_handle_t      rate_converter{};
    uint8_t                      *encoded_buffer{};
    size_t                        encoded_capacity{};
    uint8_t                      *decoded_buffer{};
    size_t                        decoded_capacity{};
    int16_t                      *channel_buffer{};
    size_t                        channel_capacity_samples{};
    int16_t                      *rate_buffer{};
    size_t                        rate_capacity_samples{};
    int16_t                      *pcm_input_buffer{};
    size_t                        pcm_input_capacity_samples{};

    audio_service_state_t          state{ AUDIO_SERVICE_STATE_UNINITIALIZED };
    audio_service_playback_state_t playback_state{ AUDIO_SERVICE_PLAYBACK_STATE_IDLE };
    esp_err_t                      last_error{ ESP_OK };
    bool                           output_active{};
    bool                           task_parked{ true };
    bool                           decoder_registered{};

    bool stop_requested{};
    bool pcm_open_requested{};
    bool pcm_close_requested{};
    bool pcm_discard_requested{};
    bool file_cancel_requested{};

    audio_service_pcm_stream_config_t requested_pcm_config{};
    audio_service_pcm_stream_config_t active_pcm_config{};
    uint32_t                          pcm_writers_active{};
    uint64_t                          requested_stream_id{};
    uint64_t                          active_stream_id{};
    uint64_t                          next_stream_id{ 1U };
    uint64_t                          last_closed_stream_id{};
    esp_err_t                         last_closed_stream_result{ ESP_OK };

    char     pending_file_path[AUDIO_SERVICE_FILE_PATH_MAX]{};
    uint64_t pending_request_id{};
    uint64_t active_request_id{};
    uint64_t last_finished_request_id{};
    uint64_t file_cancel_request_id{};

    esp_err_t control_result{ ESP_OK };
};

esp_err_t audio_service_playback_task_start(AudioServiceRuntime *runtime);
/** @brief 在持有 Runtime 锁时唤醒播放 Task。 */
void audio_service_playback_task_notify(AudioServiceRuntime *runtime);
