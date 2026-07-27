/*
 * 文件职责：封装双麦降噪录音 + 流式播放的语音交互闭环。
 * 主要依赖：audio_service、audio_processor_service、transport、voice_protocol、protocols。
 * 调用方：App 业务流程（按键触发、测试页触发）。
 *
 * 数据流：
 *   录音：(MIC) → ES7210 双通道 → 双麦 AFE 降噪/VAD → 单声道 16kHz PCM
 *   上行：WebSocket 优先，HTTP 流式接口作为连接失败回退
 *   播放：server 流式帧响应 → 逐帧解析 → TTS_PCM 帧 → 边收边播
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
        VOICE_SERVICE_EVENT_DONE,      /*!< 整个对话回合完成 */
        VOICE_SERVICE_EVENT_CANCELLED, /*!< 用户取消本轮对话 */
        VOICE_SERVICE_EVENT_ERROR,     /*!< 出错 */
    } voice_service_event_t;

    ESP_EVENT_DECLARE_BASE(VOICE_SERVICE_EVENT);

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
     * @brief 取消并有界等待活动会话退出，然后释放语音会话资源
     *
     * 本函数不释放依赖的 Audio Service，其生命周期由 Composition Root 管理。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；
     *         ESP_ERR_TIMEOUT 会话未能在内部期限内协作退出
     */
    esp_err_t voice_service_deinit(void);

    /**
     * @brief 触发一次完整的语音对话回合
     *
     * 录音 duration_ms → 上传 → 播放回复。整个流程在后台任务执行，立即返回 ESP_OK。
     * 调用方必须先确认网络在线，并持有覆盖本轮会话的产品网络租约；链路中途断开时，
     * Transport 错误会由后台任务收敛为 VOICE_SERVICE_EVENT_ERROR。
     *
     * @param[in] backend 本轮完整后端上下文，函数返回前按值复制
     * @param[in] duration_ms 录音时长（毫秒），范围 1000~10000
     * @return ESP_OK 成功，其他值表示失败
     */
    esp_err_t voice_service_chat(const protocol_backend_context_t *backend, uint32_t duration_ms);

    /** @brief 取消正在进行的语音回合。幂等；由后台任务完成资源回收。 */
    esp_err_t voice_service_cancel(void);

    /**
     * @brief 检查是否正在进行语音对话回合
     *
     * @return true 正在进行，false 空闲
     */
    bool voice_service_is_busy(void);

#ifdef __cplusplus
}
#endif
