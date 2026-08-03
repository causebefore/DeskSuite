#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_processor_service.h"
#include "esp_ae_rate_cvt.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef void (*audio_processor_feed_step_t)(void);
typedef bool (*audio_processor_fetch_step_t)(bool draining);

typedef struct
{
    bool tasks_created;
    bool feed_parked;
    bool fetch_parked;
} audio_processor_task_status_t;

/**
 * @brief Audio Processor 的私有资源 Runtime
 *
 * 该对象只在显式 init/deinit 之间存在，汇总 AFE、重采样、缓冲区、输入状态和 Task 资源。
 * 析构函数不执行阻塞清理；调用方必须先完成 stop/deinit 契约。
 */
class AudioProcessorRuntime final {
  public:
    AudioProcessorRuntime() noexcept = default;
    ~AudioProcessorRuntime() noexcept;

    AudioProcessorRuntime(const AudioProcessorRuntime &)            = delete;
    AudioProcessorRuntime &operator=(const AudioProcessorRuntime &) = delete;

    const esp_afe_sr_iface_t *afe_iface                             = nullptr;
    esp_afe_sr_data_t        *afe_data                              = nullptr;
    srmodel_list_t           *models                                = nullptr;
    int                       feed_chunksize{};
    uint32_t                  hardware_sample_rate_hz{};
    esp_ae_rate_cvt_handle_t  rate_cvt = nullptr;

    audio_processor_service_state_t state{ AUDIO_PROCESSOR_STATE_UNINITIALIZED };
    audio_processor_capture_state_t capture_state{ AUDIO_PROCESSOR_CAPTURE_IDLE };
    esp_err_t                       last_error{ ESP_OK };
    bool                            input_active{};

    int16_t *out_buf = nullptr;
    size_t   out_cap{};
    size_t   out_written{};
    bool     capture_has_speech{};
    uint32_t capture_speech_ms{};
    uint32_t capture_silence_ms{};
    size_t   capture_discard_samples{};

    int16_t *cvt_out = nullptr;
    int      cvt_out_cap{};
    int16_t *resample_buf = nullptr;
    int      resample_buf_cap{};
    int      resample_fill{};
    int16_t *feed_chunk                     = nullptr;
    int16_t *read_buf                       = nullptr;

    SemaphoreHandle_t control_lock          = nullptr;
    SemaphoreHandle_t capture_lock          = nullptr;

    EventGroupHandle_t           events     = nullptr;
    TaskHandle_t                 feed_task  = nullptr;
    TaskHandle_t                 fetch_task = nullptr;
    portMUX_TYPE                 task_lock  = portMUX_INITIALIZER_UNLOCKED;
    audio_processor_feed_step_t  feed_step  = nullptr;
    audio_processor_fetch_step_t fetch_step = nullptr;
};

AudioProcessorRuntime *audio_processor_runtime_get(void);

esp_err_t audio_processor_task_runtime_init(audio_processor_feed_step_t  feed_step,
                                            audio_processor_fetch_step_t fetch_step);
esp_err_t audio_processor_task_runtime_ensure_created(void);
void      audio_processor_task_runtime_begin_processing(void);
void      audio_processor_task_runtime_begin_drain(void);
esp_err_t audio_processor_task_runtime_wait_drain_and_park(uint32_t timeout_ms);
esp_err_t audio_processor_task_runtime_park(uint32_t timeout_ms);
esp_err_t audio_processor_task_runtime_deinit(uint32_t timeout_ms);
void      audio_processor_task_runtime_get_status(audio_processor_task_status_t *out_status);
