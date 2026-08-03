#include "voice_service_internal.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

namespace {

constexpr uint32_t    kVoiceConversationTaskStackBytes = 12288U;
constexpr UBaseType_t kVoiceConversationTaskPriority   = 2U;

const char *TAG                                        = "voice_task";

/** @brief 连续语音会话 Task 入口，仅负责调用 Runtime 业务函数和清除句柄。 */
void voice_conversation_task(void *arg)
{
    auto *runtime = static_cast<VoiceServiceRuntime *>(arg);
    voice_service_run_conversation(runtime);
    const UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(nullptr);
    taskENTER_CRITICAL(&runtime->session_lock);
    runtime->conversation_task = nullptr;
    taskEXIT_CRITICAL(&runtime->session_lock);
    ESP_LOGI(TAG, "语音会话 Task 已退出，栈高水位=%u 字节", (unsigned) stack_high_water);
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

esp_err_t voice_service_conversation_task_start(VoiceServiceRuntime *runtime)
{
    if (runtime == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&runtime->session_lock);
    const bool task_active = runtime->conversation_task != nullptr;
    taskEXIT_CRITICAL(&runtime->session_lock);
    if (task_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t result = xTaskCreateWithCaps(voice_conversation_task,
                                                  "voice_conversation",
                                                  kVoiceConversationTaskStackBytes,
                                                  runtime,
                                                  kVoiceConversationTaskPriority,
                                                  &runtime->conversation_task,
                                                  MALLOC_CAP_SPIRAM);
    if (result != pdPASS)
    {
        taskENTER_CRITICAL(&runtime->session_lock);
        runtime->conversation_task = nullptr;
        taskEXIT_CRITICAL(&runtime->session_lock);
    }
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool voice_service_conversation_task_active(VoiceServiceRuntime *runtime)
{
    if (runtime == nullptr)
    {
        return false;
    }
    taskENTER_CRITICAL(&runtime->session_lock);
    const bool active = runtime->conversation_task != nullptr;
    taskEXIT_CRITICAL(&runtime->session_lock);
    return active;
}
