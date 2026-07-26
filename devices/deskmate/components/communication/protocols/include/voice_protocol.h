/**
 * @file voice_protocol.h
 * @brief 定义 Voice 帧语义、流式解码器和 WebSocket 控制消息
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 单帧允许的最大负载字节数 */
#define VOICE_PROTOCOL_MAX_PAYLOAD 65536
/** @brief Voice 帧头固定字节数 */
#define VOICE_PROTOCOL_HEADER_SIZE 5

    /** @brief Voice 下行帧类型 */
    typedef enum
    {
        VOICE_PROTOCOL_FRAME_END        = 0x00, /**< 会话响应结束 */
        VOICE_PROTOCOL_FRAME_ASR_TEXT   = 0x01, /**< 语音识别文本 */
        VOICE_PROTOCOL_FRAME_REPLY_TEXT = 0x02, /**< 助手回复文本 */
        VOICE_PROTOCOL_FRAME_TTS_PCM    = 0x03, /**< TTS PCM 音频 */
        VOICE_PROTOCOL_FRAME_THINKING   = 0x04, /**< 服务端处理中 */
        VOICE_PROTOCOL_FRAME_ERROR      = 0x80, /**< 服务端错误文本 */
    } voice_protocol_frame_type_t;

    /** @brief 可跨输入分片保留未完成帧的 Voice 解码器 */
    typedef struct voice_protocol_decoder voice_protocol_decoder_t;

    /**
     * @brief 接收一帧已经组装完成的 Voice 数据
     *
     * 回调由 voice_protocol_decoder_feed() 在其调用上下文同步执行。payload 指向解码器内部
     * 缓冲区，仅在本次回调期间有效；需要跨调用保存时必须复制。回调不得重入、销毁或并发
     * 操作当前解码器。
     *
     * @param[in] type 帧类型
     * @param[in] payload 帧负载；len 为 0 时不得解引用
     * @param[in] len 帧负载字节数
     * @param[in] ctx 调用方上下文
     */
    typedef void (*voice_protocol_frame_cb_t)(voice_protocol_frame_type_t type, const uint8_t *payload, size_t len,
                                              void *ctx);

    /**
     * @brief 创建空的 Voice 流式解码器
     *
     * 成功后解码器所有权交给调用方，必须使用 voice_protocol_decoder_destroy() 释放。
     *
     * @return 解码器指针；PSRAM 分配失败时返回 NULL
     */
    voice_protocol_decoder_t *voice_protocol_decoder_create(void);

    /**
     * @brief 销毁 Voice 流式解码器
     *
     * @param[in] decoder 待销毁解码器；可为 NULL
     */
    void voice_protocol_decoder_destroy(voice_protocol_decoder_t *decoder);

    /**
     * @brief 同步消费一段 Voice 字节流并报告其中的完整帧
     *
     * 不完整帧会保留在 decoder 中供后续调用继续组装。函数仅在调用期间借用 data、callback
     * 和 ctx；同一 decoder 的 feed 调用必须由调用方串行化。
     *
     * @param[in,out] decoder 流式解码状态
     * @param[in] data 本次输入字节
     * @param[in] len 本次输入字节数
     * @param[in] callback 完整帧同步回调
     * @param[in] ctx 回调上下文
     * @return ESP_OK 本段输入已消费；ESP_ERR_INVALID_ARG 参数无效；
     *         ESP_ERR_INVALID_SIZE 帧声明的负载超过上限且解码器已复位
     */
    esp_err_t voice_protocol_decoder_feed(voice_protocol_decoder_t *decoder, const uint8_t *data, size_t len,
                                          voice_protocol_frame_cb_t callback, void *ctx);

    /**
     * @brief 取得 Voice 会话开始控制消息
     *
     * @return 进程期有效的只读 JSON 字符串，不得释放或修改
     */
    const char *voice_protocol_start_message(void);

    /**
     * @brief 取得 Voice 输入结束控制消息
     *
     * @return 进程期有效的只读 JSON 字符串，不得释放或修改
     */
    const char *voice_protocol_end_input_message(void);

    /**
     * @brief 取得 Voice 会话取消控制消息
     *
     * @return 进程期有效的只读 JSON 字符串，不得释放或修改
     */
    const char *voice_protocol_cancel_message(void);

#ifdef __cplusplus
}
#endif
