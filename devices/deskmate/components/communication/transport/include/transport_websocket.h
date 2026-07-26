/**
 * @file transport_websocket.h
 * @brief 提供原始 WebSocket 连接、完整消息收发与分片组装能力
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief Transport 独占资源的不透明 WebSocket 对象 */
    typedef struct transport_websocket transport_websocket_t;

    /** @brief WebSocket 连接与接收资源配置 */
    typedef struct
    {
        const char *url;                  /**< WebSocket URL */
        const char *headers;              /**< 可为空的附加 HTTP 请求头文本 */
        int         network_timeout_ms;   /**< 底层网络操作超时，单位毫秒 */
        int         connect_timeout_ms;   /**< 等待首次连接结果的超时，单位毫秒 */
        size_t      receive_buffer_bytes; /**< 底层单次接收缓冲区字节数 */
        size_t      max_message_bytes;    /**< 分片组装后的单消息最大字节数 */
        size_t      receive_queue_length; /**< 完整消息接收队列容量 */
    } transport_websocket_config_t;

    /** @brief 从 WebSocket 接收队列取得的完整消息及其内存所有权 */
    typedef struct
    {
        uint8_t *data;   /**< 调用方持有的消息数据，使用后必须释放 */
        size_t   len;    /**< 消息字节数 */
        bool     binary; /**< true 为二进制消息；false 为文本消息 */
    } transport_websocket_message_t;

    /**
     * @brief 同步创建 WebSocket 对象并等待首次连接结果
     *
     * 函数在返回前复制 config 中的 URL 与请求头，不保留 config 指针。返回 ESP_OK 时对象
     * 所有权交给调用方，必须使用 transport_websocket_close() 关闭并销毁。
     *
     * @param[in] config 连接与接收资源配置
     * @param[out] out 成功时返回 WebSocket 对象
     * @return ESP_OK 已连接；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NO_MEM 资源创建失败；
     *         ESP_ERR_TIMEOUT 首次连接超时；或其他底层连接错误码
     */
    esp_err_t transport_websocket_open(const transport_websocket_config_t *config, transport_websocket_t **out);

    /**
     * @brief 同步发送一条完整文本消息
     *
     * 函数仅在调用期间借用 text，不保存其指针。
     *
     * @param[in] socket WebSocket 对象
     * @param[in] text 以空字符结尾的文本消息
     * @param[in] timeout_ms 最长发送时间，单位毫秒
     * @return ESP_OK 已完整发送；ESP_ERR_INVALID_ARG 参数无效；ESP_FAIL 发送不完整；
     *         或其他底层发送错误码
     */
    esp_err_t transport_websocket_send_text(transport_websocket_t *socket, const char *text, uint32_t timeout_ms);

    /**
     * @brief 同步发送一条完整二进制消息
     *
     * 函数仅在调用期间借用 data，不保存其指针。
     *
     * @param[in] socket WebSocket 对象
     * @param[in] data 二进制消息数据
     * @param[in] len 消息字节数，必须大于 0
     * @param[in] timeout_ms 最长发送时间，单位毫秒
     * @return ESP_OK 已完整发送；ESP_ERR_INVALID_ARG 参数无效；ESP_FAIL 发送不完整；
     *         或其他底层发送错误码
     */
    esp_err_t transport_websocket_send_binary(transport_websocket_t *socket, const uint8_t *data, size_t len,
                                              uint32_t timeout_ms);

    /**
     * @brief 同步等待并取得一条已经完成分片组装的消息
     *
     * 返回 ESP_OK 时消息数据所有权转移给调用方，使用后必须调用
     * transport_websocket_message_release()。参数校验通过后 message 会先清零。
     *
     * @param[in] socket WebSocket 对象
     * @param[out] message 完整消息输出
     * @param[in] timeout_ms 最长等待时间，单位毫秒
     * @return ESP_OK 已收到；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_TIMEOUT 等待超时；
     *         ESP_ERR_INVALID_STATE 连接已经断开
     */
    esp_err_t transport_websocket_receive(transport_websocket_t *socket, transport_websocket_message_t *message,
                                          uint32_t timeout_ms);

    /**
     * @brief 查询 WebSocket 当前连接事实
     *
     * @param[in] socket WebSocket 对象；可为 NULL
     * @return true 当前已连接；false 未连接或对象为空
     */
    bool transport_websocket_is_connected(const transport_websocket_t *socket);

    /**
     * @brief 查询因容量或队列限制而累计丢弃的完整消息数
     *
     * @param[in] socket WebSocket 对象；可为 NULL
     * @return 累计丢弃消息数；对象为空时返回 0
     */
    uint32_t transport_websocket_dropped_messages(const transport_websocket_t *socket);

    /**
     * @brief 释放接收消息拥有的数据并清空结构
     *
     * message 为 NULL 或结构已经清空时保持幂等。
     *
     * @param[in,out] message 待释放消息
     */
    void transport_websocket_message_release(transport_websocket_message_t *message);

    /**
     * @brief 同步关闭连接并销毁 WebSocket 对象
     *
     * 调用前必须结束针对同一对象的发送、接收和状态查询；函数会释放队列内尚未取出的消息。
     * socket 为 NULL 时保持幂等。
     *
     * @param[in] socket 待关闭对象
     */
    void transport_websocket_close(transport_websocket_t *socket);

#ifdef __cplusplus
}
#endif
