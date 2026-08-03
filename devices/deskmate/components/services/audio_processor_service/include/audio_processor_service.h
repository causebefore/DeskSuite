/*
 * 文件职责：封装 ESP-SR AFE 双麦空间增强、降噪、VAD 与可选唤醒检测。
 * 主要依赖：device_audio、esp-sr（AFE NS 模型）、esp_audio_effects（硬件采样率→16kHz 重采样）。
 * 调用方：voice_service 录音流程。
 *
 * 数据流：
 *   输入：ES7210 双通道硬件采样率交错 PCM
 *     -> 提取双麦 -> 按需重采样至 16kHz
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

    /** @brief AFE Service 可逆运行状态。 */
    typedef enum
    {
        AUDIO_PROCESSOR_STATE_UNINITIALIZED = 0, /*!< 尚未初始化 */
        AUDIO_PROCESSOR_STATE_STOPPED,           /*!< 资源保留，拒绝采集 */
        AUDIO_PROCESSOR_STATE_RUNNING,           /*!< 允许按需采集 */
        AUDIO_PROCESSOR_STATE_STOPPING,          /*!< 正在等待处理 Task 停泊 */
        AUDIO_PROCESSOR_STATE_CLEANUP_FAILED,    /*!< 停泊不完整，只允许继续 stop 收敛 */
    } audio_processor_service_state_t;

    /** @brief 单次 AFE 收集事务状态。 */
    typedef enum
    {
        AUDIO_PROCESSOR_CAPTURE_IDLE = 0,  /*!< 无活动收集 */
        AUDIO_PROCESSOR_CAPTURE_CAPTURING, /*!< 正在采集 */
        AUDIO_PROCESSOR_CAPTURE_DRAINING,  /*!< 已停 feed，正在排空 AFE */
    } audio_processor_capture_state_t;

    /** @brief AFE Service 的有界运行摘要。 */
    typedef struct
    {
        audio_processor_service_state_t state;         /*!< Runtime 状态 */
        audio_processor_capture_state_t capture_state; /*!< 收集状态 */
        bool                            tasks_created; /*!< 是否已经创建处理 Task */
        bool                            feed_parked;   /*!< feed Task 未创建或已阻塞 */
        bool                            fetch_parked;  /*!< fetch Task 未创建或已阻塞 */
        bool                            input_active;  /*!< 麦克风输入链路是否已开启 */
        esp_err_t                       last_error;    /*!< 最近生命周期错误 */
    } audio_processor_service_status_t;

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
     * 调用前必须已初始化 device_audio；成功后可重复 capture_start/capture_stop。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化或依赖尚未初始化；其他值表示失败
     */
    esp_err_t audio_processor_service_init(void);

    /**
     * @brief 可逆启动 AFE Service
     *
     * WakeNet 开启时同步启用麦克风并开始常驻检测；WakeNet 关闭时等待 capture_start 按需启用输入。
     *
     * @return ESP_OK 已进入 RUNNING；ESP_ERR_INVALID_STATE 生命周期不允许
     */
    esp_err_t audio_processor_service_start(void);

    /**
     * @brief 同步等待 AFE Task 停泊并可逆停止 Service
     *
     * 活动收集或 drain 期间不会强制取消，直接返回 ESP_ERR_INVALID_STATE。
     *
     * @param[in] timeout_ms 等待 Task 停泊的最长时间
     * @return ESP_OK 已进入 STOPPED；ESP_ERR_INVALID_ARG 超时为零；
     *         ESP_ERR_INVALID_STATE 生命周期或收集状态不允许；ESP_ERR_TIMEOUT Task 未停泊
     */
    esp_err_t audio_processor_service_stop(uint32_t timeout_ms);

    /**
     * @brief 停止常驻处理 Task 并释放模型、AFE、重采样器和缓冲资源
     *
     * 调用前不得存在活动收集会话。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或仍在收集；
     *         ESP_ERR_TIMEOUT Task 未能在内部期限内协作退出
     */
    esp_err_t audio_processor_service_deinit(void);

    /**
     * @brief 查询音频处理 Service 是否已经初始化
     * @return true 已初始化；false 尚未初始化
     */
    bool audio_processor_service_is_initialized(void);

    /**
     * @brief 复制 AFE Service 完整运行摘要
     *
     * @param[out] out_status 运行摘要输出
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t audio_processor_service_get_status_copy(audio_processor_service_status_t *out_status);

    /**
     * @brief 进入降噪 PCM 收集模式
     *
     * 从 RUNNING + IDLE 进入 CAPTURING，重置缓冲并按需启动 feed/fetch 任务。
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
     * 触发 fetch 任务排空 AFE 管线残余数据，等待 Task 停泊后回到 IDLE。
     *
     * @param[out] out_sample_count 实际写入 out_buf 的 int16 样本总数
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 无活动会话；ESP_ERR_TIMEOUT 排空超时
     */
    esp_err_t audio_processor_service_capture_stop(size_t *out_sample_count);

    /**
     * @brief 当前收集回合是否已检测到人声
     * @return true 已检测到满足最短时长的人声；false 尚未检测到
     */
    bool audio_processor_service_capture_has_speech(void);

    /**
     * @brief 当前回合连续静音时长（毫秒，近似值）
     * @return 连续静音毫秒数；尚未初始化时为 0
     */
    uint32_t audio_processor_service_capture_silence_ms(void);

    /**
     * @brief 清除当前回合的 VAD 活动标记，用于忽略唤醒词尾音
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或当前不在采集
     */
    esp_err_t audio_processor_service_capture_reset_activity(void);

#ifdef __cplusplus
}
#endif
