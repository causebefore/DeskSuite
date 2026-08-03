/*
 * 文件职责：封装双麦降噪录音、网络会话与 TTS PCM 提交的语音交互闭环。
 * 主要依赖：audio_service、audio_processor_service、transport、voice_protocol、protocols。
 * 调用方：App 业务流程（按键或唤醒词触发）。
 *
 * 数据流：
 *   录音：(MIC) → ES7210 双通道 → 双麦 AFE 降噪/VAD → 单声道 16kHz PCM
 *   上行：WebSocket 优先，HTTP 流式接口作为连接失败回退
 *   播放：server 流式帧响应 → 逐帧解析 → Audio Service 唯一 PCM 输出事务
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "protocol_backend_context.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 语音服务事件类型。 */
    typedef enum
    {
        VOICE_SERVICE_EVENT_NONE = 0,  /*!< 无事件 */
        VOICE_SERVICE_EVENT_RECORDING, /*!< 开始录音 */
        VOICE_SERVICE_EVENT_THINKING,  /*!< 录音结束，等待 server 响应 */
        VOICE_SERVICE_EVENT_SPEAKING,  /*!< 收到响应，开始播放 */
        VOICE_SERVICE_EVENT_DONE,      /*!< 连续会话正常结束 */
        VOICE_SERVICE_EVENT_CANCELLED, /*!< 用户取消连续会话 */
        VOICE_SERVICE_EVENT_ERROR,     /*!< 出错 */
        VOICE_SERVICE_EVENT_NO_SPEECH, /*!< 首轮前导窗口内未检测到有效人声 */
    } voice_service_event_t;

    ESP_EVENT_DECLARE_BASE(VOICE_SERVICE_EVENT);

    /** @brief 语音 Service 可逆运行状态。 */
    typedef enum
    {
        VOICE_SERVICE_STATE_UNINITIALIZED = 0, /*!< 尚未初始化 */
        VOICE_SERVICE_STATE_STOPPED,           /*!< 资源保留，拒绝新会话 */
        VOICE_SERVICE_STATE_RUNNING,           /*!< 允许提交语音会话 */
        VOICE_SERVICE_STATE_STOPPING,          /*!< 正在关闭会话入口 */
        VOICE_SERVICE_STATE_CLEANUP_FAILED,    /*!< 停止不完整，只允许继续收敛 */
    } voice_service_state_t;

    /** @brief 语音 Service 的有界运行摘要。 */
    typedef struct
    {
        voice_service_state_t state;                    /*!< 生命周期状态 */
        bool                  session_busy;             /*!< 是否存在活动连续会话 */
        bool                  conversation_task_active; /*!< 连续会话 Task 是否仍存在 */
        esp_err_t             last_error;               /*!< 最近生命周期错误 */
    } voice_service_status_t;

    /**
     * @brief 初始化语音服务
     *
     * 调用前必须已由 Composition Root 初始化 audio_service 与
     * audio_processor_service；本函数只创建语音会话自身资源。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化或依赖尚未初始化；其他值表示失败
     */
    esp_err_t voice_service_init(void);

    /**
     * @brief 可逆启动语音 Service 并开放新会话入口
     *
     * @return ESP_OK 已进入 RUNNING；ESP_ERR_INVALID_STATE 生命周期不允许
     */
    esp_err_t voice_service_start(void);

    /**
     * @brief 在会话空闲时关闭新会话入口并进入 STOPPED
     *
     * 本函数不会取消活动会话。
     *
     * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE 尚未初始化、会话活动或生命周期不允许
     */
    esp_err_t voice_service_stop(void);

    /**
     * @brief 从 STOPPED 释放语音会话资源
     *
     * 本函数不释放依赖的 Audio Service，其生命周期由 Composition Root 管理。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化、未停止或 Task 仍存在
     */
    esp_err_t voice_service_deinit(void);

    /**
     * @brief 复制语音 Service 完整运行摘要
     *
     * @param[out] out_status 运行摘要输出
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t voice_service_get_status_copy(voice_service_status_t *out_status);

    /**
     * @brief 复制后端上下文并异步启动一次连续语音会话
     *
     * 每个回合依次录音 chat_duration_ms、上传并完整播放回复；回复 PCM 排空后自动开始下一轮录音。
     * 首轮前导窗口内没有有效人声时以 VOICE_SERVICE_EVENT_NO_SPEECH 结束；至少完成一个回合后，
     * 后续等待达到 followup_timeout_ms 仍没有有效人声时以 VOICE_SERVICE_EVENT_DONE 正常结束。
     * 整个流程在后台 Task 执行，本函数返回前复制完整后端上下文。调用方必须先确认网络在线，
     * 并持有覆盖整段连续会话的产品网络租约；链路中途断开时由后台 Task 收敛为 ERROR。
     *
     * @param[in] backend 连续会话使用的完整后端上下文
     * @param[in] chat_duration_ms 单轮最长录音时长（毫秒），自动限制为 2000～10000
     * @param[in] followup_timeout_ms 回复后等待下一句有效人声的时长（毫秒），范围 1000～10000；
     *            大于单轮录音时长时按单轮录音时长执行
     * @return ESP_OK 请求已接受；ESP_ERR_INVALID_ARG 参数无效；
     *         ESP_ERR_INVALID_STATE Service 未运行或已有会话；或资源创建错误码
     */
    esp_err_t voice_service_request_conversation_copy(const protocol_backend_context_t *backend,
                                                      uint32_t chat_duration_ms, uint32_t followup_timeout_ms);

    /**
     * @brief 取消正在进行的连续语音会话
     *
     * 幂等；由后台任务完成资源回收。
     *
     * @return ESP_OK 已记录或当前空闲；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t voice_service_cancel(void);

    /**
     * @brief 检查是否正在进行连续语音会话
     *
     * @return true 正在进行，false 空闲
     */
    bool voice_service_is_busy(void);

#ifdef __cplusplus
}
#endif
