#include "voice_protocol.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

struct voice_protocol_decoder
{
    uint8_t  header[VOICE_PROTOCOL_HEADER_SIZE];
    size_t   header_pos;
    uint8_t  payload[VOICE_PROTOCOL_MAX_PAYLOAD];
    uint32_t payload_len;
    size_t   payload_pos;
};

static void decoder_reset(voice_protocol_decoder_t *decoder)
{
    decoder->header_pos  = 0;
    decoder->payload_len = 0;
    decoder->payload_pos = 0;
}

voice_protocol_decoder_t *voice_protocol_decoder_create(void)
{
    voice_protocol_decoder_t *decoder = heap_caps_calloc(1, sizeof(*decoder), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return decoder;
}

void voice_protocol_decoder_destroy(voice_protocol_decoder_t *decoder)
{
    free(decoder);
}

esp_err_t voice_protocol_decoder_feed(voice_protocol_decoder_t *decoder, const uint8_t *data, size_t len,
                                      voice_protocol_frame_cb_t callback, void *ctx)
{
    if (decoder == NULL || data == NULL || callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    size_t pos = 0;
    while (pos < len)
    {
        if (decoder->header_pos < VOICE_PROTOCOL_HEADER_SIZE)
        {
            const size_t needed = VOICE_PROTOCOL_HEADER_SIZE - decoder->header_pos;
            const size_t take   = needed < len - pos ? needed : len - pos;
            memcpy(decoder->header + decoder->header_pos, data + pos, take);
            decoder->header_pos += take;
            pos += take;
            if (decoder->header_pos == VOICE_PROTOCOL_HEADER_SIZE)
            {
                decoder->payload_len = ((uint32_t) decoder->header[1] << 24) | ((uint32_t) decoder->header[2] << 16)
                                       | ((uint32_t) decoder->header[3] << 8) | (uint32_t) decoder->header[4];
                if (decoder->payload_len > VOICE_PROTOCOL_MAX_PAYLOAD)
                {
                    decoder_reset(decoder);
                    return ESP_ERR_INVALID_SIZE;
                }
                if (decoder->payload_len == 0)
                {
                    callback((voice_protocol_frame_type_t) decoder->header[0], decoder->payload, 0, ctx);
                    decoder_reset(decoder);
                }
            }
        }
        else
        {
            const size_t needed = decoder->payload_len - decoder->payload_pos;
            const size_t take   = needed < len - pos ? needed : len - pos;
            memcpy(decoder->payload + decoder->payload_pos, data + pos, take);
            decoder->payload_pos += take;
            pos += take;
            if (decoder->payload_pos == decoder->payload_len)
            {
                callback((voice_protocol_frame_type_t) decoder->header[0], decoder->payload, decoder->payload_len, ctx);
                decoder_reset(decoder);
            }
        }
    }
    return ESP_OK;
}

const char *voice_protocol_start_message(void)
{
    return "{\"type\":\"start\",\"codec\":\"pcm_s16le\",\"sample_rate\":16000,\"channels\":1}";
}

const char *voice_protocol_end_input_message(void)
{
    return "{\"type\":\"end_input\"}";
}

const char *voice_protocol_cancel_message(void)
{
    return "{\"type\":\"cancel\"}";
}
