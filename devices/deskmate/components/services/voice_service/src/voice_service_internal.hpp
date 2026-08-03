#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "protocol_backend_context.h"
#include "voice_service.h"

/**
 * @brief Voice Service 私有会话 Runtime
 *
 * Runtime 本体必须放在内部 RAM，确保 PSRAM 栈上的会话 Task 触发 Flash/NVS 临界区时仍可读取
 * 已复制的后端上下文。析构函数只验证显式清理结果，不执行阻塞操作。
 */
class VoiceServiceRuntime final {
  public:
    VoiceServiceRuntime() noexcept = default;
    ~VoiceServiceRuntime() noexcept;

    VoiceServiceRuntime(const VoiceServiceRuntime &)            = delete;
    VoiceServiceRuntime &operator=(const VoiceServiceRuntime &) = delete;

    voice_service_state_t state{ VOICE_SERVICE_STATE_UNINITIALIZED };
    bool                  busy{};
    esp_err_t             last_error{ ESP_OK };

    StaticEventGroup_t session_events_storage{};
    EventGroupHandle_t session_events{};
    portMUX_TYPE       session_lock = portMUX_INITIALIZER_UNLOCKED;

    protocol_backend_context_t conversation_backend{};
    uint32_t                   chat_duration_ms{};
    uint32_t                   followup_timeout_ms{};
    TaskHandle_t               conversation_task{};
    int16_t                   *record_buffer{};
    size_t                     record_capacity_samples{};
};

void      voice_service_run_conversation(VoiceServiceRuntime *runtime);
esp_err_t voice_service_conversation_task_start(VoiceServiceRuntime *runtime);
bool      voice_service_conversation_task_active(VoiceServiceRuntime *runtime);
