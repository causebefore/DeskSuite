/* 文件职责：定义 Voice 帧语义、流式解码器和 WebSocket 控制消息。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define VOICE_PROTOCOL_MAX_PAYLOAD 65536
#define VOICE_PROTOCOL_HEADER_SIZE 5

    typedef enum
    {
        VOICE_PROTOCOL_FRAME_END        = 0x00,
        VOICE_PROTOCOL_FRAME_ASR_TEXT   = 0x01,
        VOICE_PROTOCOL_FRAME_REPLY_TEXT = 0x02,
        VOICE_PROTOCOL_FRAME_TTS_PCM    = 0x03,
        VOICE_PROTOCOL_FRAME_THINKING   = 0x04,
        VOICE_PROTOCOL_FRAME_ERROR      = 0x80,
    } voice_protocol_frame_type_t;

    typedef struct voice_protocol_decoder voice_protocol_decoder_t;
    typedef void (*voice_protocol_frame_cb_t)(voice_protocol_frame_type_t type,
                                              const uint8_t *payload, size_t len, void *ctx);

    voice_protocol_decoder_t *voice_protocol_decoder_create(void);
    void                      voice_protocol_decoder_destroy(voice_protocol_decoder_t *decoder);
    esp_err_t voice_protocol_decoder_feed(voice_protocol_decoder_t *decoder, const uint8_t *data,
                                          size_t len, voice_protocol_frame_cb_t callback,
                                          void *ctx);

    const char *voice_protocol_start_message(void);
    const char *voice_protocol_end_input_message(void);
    const char *voice_protocol_cancel_message(void);

#ifdef __cplusplus
}
#endif
