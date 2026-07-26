/* 文件职责：提供原始 WebSocket 连接、完整消息收发与分片组装能力。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct transport_websocket transport_websocket_t;

    typedef struct
    {
        const char *url;
        const char *headers;
        int         network_timeout_ms;
        int         connect_timeout_ms;
        size_t      receive_buffer_bytes;
        size_t      max_message_bytes;
        size_t      receive_queue_length;
    } transport_websocket_config_t;

    typedef struct
    {
        uint8_t *data;
        size_t   len;
        bool     binary;
    } transport_websocket_message_t;

    esp_err_t transport_websocket_open(const transport_websocket_config_t *config,
                                       transport_websocket_t             **out);
    esp_err_t transport_websocket_send_text(transport_websocket_t *socket, const char *text,
                                            uint32_t timeout_ms);
    esp_err_t transport_websocket_send_binary(transport_websocket_t *socket, const uint8_t *data,
                                              size_t len, uint32_t timeout_ms);
    esp_err_t transport_websocket_receive(transport_websocket_t         *socket,
                                          transport_websocket_message_t *message,
                                          uint32_t                       timeout_ms);
    bool      transport_websocket_is_connected(const transport_websocket_t *socket);
    uint32_t  transport_websocket_dropped_messages(const transport_websocket_t *socket);
    void      transport_websocket_message_release(transport_websocket_message_t *message);
    void      transport_websocket_close(transport_websocket_t *socket);

#ifdef __cplusplus
}
#endif
