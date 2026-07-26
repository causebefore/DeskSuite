/*
 * 文件职责：封装 ESP-SR AFE 双麦空间增强、降噪、VAD 与可选唤醒检测。
 * 主要依赖：esp-sr（AFE NS 模型）、esp_audio_effects（24kHz→16kHz 重采样）。
 * 调用方：voice_service 录音流程。
 *
 * 数据流：
 *   输入：ES7210 双通道 24kHz 交错 PCM
 *     -> 提取双麦 -> 24kHz->16kHz 重采样
 *     -> ESP-SR AFE (SE + NS + VAD，可选 WakeNet) -> 单声道 16kHz PCM
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 音频处理器事件（唤醒检测上报）
     *
     * Service → App 走事件，Service 不直接调用 App。
     */
    typedef enum
    {
        AUDIO_PROCESSOR_EVENT_WAKE = 0, /* 检测到唤醒词（raw，未仲裁） */
    } audio_processor_event_t;

    ESP_EVENT_DECLARE_BASE(AUDIO_PROCESSOR_EVENT);

    /**
     * @brief 初始化 AFE 双麦降噪处理器
     *
     * 创建 AFE 实例（"MM" 双麦格式，启用 SE、NS、VAD 与可选 WakeNet 模型）。
     * 调用前必须已初始化 audio_service；成功后可重复 capture_start/capture_stop。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化或依赖尚未初始化；其他值表示失败
     */
    esp_err_t audio_processor_service_init(void);

    /**
     * @brief 停止常驻处理 Task 并释放模型、AFE、重采样器和缓冲资源
     *
     * 调用前不得存在活动收集会话。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或仍在收集；
     *         ESP_ERR_TIMEOUT Task 未能在内部期限内协作退出
     */
    esp_err_t audio_processor_service_deinit(void);

    /** @brief 查询音频处理 Service 是否已经初始化。 */
    bool audio_processor_service_is_initialized(void);

    /**
     * @brief 进入降噪 PCM 收集模式
     *
     * 置 collecting=true，重置缓冲；默认配置下按需启动 feed/fetch 任务。
     * fetch 任务会把降噪后的 16kHz 单声道 PCM 写入 out_buf。
     *
     * @param[in] out_buf 输出缓冲（int16 数组，建议放 PSRAM）
     * @param[in] out_cap 缓冲容量（int16 样本数）
     * @return ESP_OK 成功
     */
    esp_err_t audio_processor_service_capture_start(int16_t *out_buf, size_t out_cap);

    /**
     * @brief 结束收集（drain 管线），返回收集到的样本数
     *
     * 触发 fetch 任务排空 AFE 管线残余数据后清 collecting。
     *
     * @param[out] out_sample_count 实际写入 out_buf 的 int16 样本总数
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 无活动会话；ESP_ERR_TIMEOUT 排空超时
     */
    esp_err_t audio_processor_service_capture_stop(size_t *out_sample_count);

    /** @brief 当前收集回合是否已检测到人声。 */
    bool audio_processor_service_capture_has_speech(void);

    /** @brief 当前回合连续静音时长（毫秒，近似值）。 */
    uint32_t audio_processor_service_capture_silence_ms(void);

    /** @brief 清除当前回合的 VAD 活动标记，用于忽略唤醒词尾音。 */
    esp_err_t audio_processor_service_capture_reset_activity(void);

#ifdef __cplusplus
}
#endif
