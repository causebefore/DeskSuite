/**
 * @file transport_websocket.cpp
 * @brief 使用 C++ 对象管理 WebSocket 连接、队列和分片缓冲区
 */
#include "transport_websocket.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static_assert(std::is_trivially_copyable_v<transport_websocket_message_t>,
              "WebSocket 队列消息必须可按值复制");

/** @brief 日志标签 */
static const char *TAG = "communication_ws";

/** @brief 管理单个 WebSocket 连接拥有的全部资源 */
struct transport_websocket
{
    /** @brief 初始化空 WebSocket 对象 */
    transport_websocket() = default;

    /** @brief 关闭连接并释放对象拥有的队列、信号量和缓冲区 */
    ~transport_websocket()
    {
        if (client != nullptr)
        {
            if (esp_websocket_client_is_connected(client))
            {
                (void) esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
            }
            if (events_registered)
            {
                (void) esp_websocket_unregister_events(client, WEBSOCKET_EVENT_ANY, handle_event);
            }
            (void) esp_websocket_client_destroy(client);
        }

        transport_websocket_message_t pending = {};
        if (receive_queue != nullptr)
        {
            while (xQueueReceive(receive_queue, &pending, 0) == pdTRUE)
            {
                free(pending.data);
                pending = {};
            }
            vQueueDelete(receive_queue);
        }
        if (connection_signal != nullptr)
        {
            vSemaphoreDelete(connection_signal);
        }
        free(assemble_data);
        ESP_LOGD(TAG, "WebSocket 已关闭");
    }

    /** @brief 禁止复制 WebSocket 资源所有权 */
    transport_websocket(const transport_websocket &)            = delete;

    /** @brief 禁止复制 WebSocket 资源所有权 */
    transport_websocket &operator=(const transport_websocket &) = delete;

    /**
     * @brief 创建底层资源并等待连接结果
     *
     * @param[in] config WebSocket 配置
     * @return ESP_OK 已连接；其他值为初始化或连接错误码
     */
    esp_err_t open(const transport_websocket_config_t &config)
    {
        max_message_bytes = config.max_message_bytes;
        receive_queue =
            xQueueCreate(config.receive_queue_length, sizeof(transport_websocket_message_t));
        connection_signal = xSemaphoreCreateBinary();
        if (receive_queue == nullptr || connection_signal == nullptr)
        {
            return ESP_ERR_NO_MEM;
        }

        esp_websocket_client_config_t websocket_config = {};
        websocket_config.uri                           = config.url;
        websocket_config.headers                       = config.headers;
        websocket_config.disable_auto_reconnect        = true;
        websocket_config.buffer_size        = static_cast<int>(config.receive_buffer_bytes);
        websocket_config.network_timeout_ms = config.network_timeout_ms;
        client                              = esp_websocket_client_init(&websocket_config);
        if (client == nullptr)
        {
            return ESP_ERR_NO_MEM;
        }

        esp_err_t err =
            esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, handle_event, this);
        events_registered = err == ESP_OK;
        if (err == ESP_OK)
        {
            err = esp_websocket_client_start(client);
        }
        if (err == ESP_OK
            && (xSemaphoreTake(connection_signal, pdMS_TO_TICKS(config.connect_timeout_ms))
                    != pdTRUE
                || !connected))
        {
            err = ESP_ERR_TIMEOUT;
        }
        return err;
    }

    /**
     * @brief 接收 ESP WebSocket 事件并转交给对象
     *
     * @param[in] arg WebSocket 对象
     * @param[in] base 事件基类，本实现不使用
     * @param[in] id 事件编号
     * @param[in] data 事件数据
     */
    static void handle_event(void *arg, esp_event_base_t base, int32_t id, void *data)
    {
        (void) base;
        auto *socket = static_cast<transport_websocket *>(arg);
        if (socket != nullptr)
        {
            socket->on_event(id, static_cast<esp_websocket_event_data_t *>(data));
        }
    }

    /**
     * @brief 更新连接状态或组装一条完整消息
     *
     * @param[in] id 事件编号
     * @param[in] event WebSocket 事件数据
     */
    void on_event(int32_t id, esp_websocket_event_data_t *event)
    {
        if (id == WEBSOCKET_EVENT_CONNECTED)
        {
            connected    = true;
            disconnected = false;
            xSemaphoreGive(connection_signal);
            return;
        }
        if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR)
        {
            connected    = false;
            disconnected = true;
            xSemaphoreGive(connection_signal);
            return;
        }
        if (id != WEBSOCKET_EVENT_DATA || event == nullptr || event->data_len <= 0)
        {
            return;
        }

        const bool binary = event->op_code == WS_TRANSPORT_OPCODES_BINARY
                            || event->op_code == WS_TRANSPORT_OPCODES_CONT;
        if (event->payload_offset == 0)
        {
            reset_assembly();
            if (event->payload_len <= 0
                || static_cast<size_t>(event->payload_len) > max_message_bytes)
            {
                ++dropped_messages;
                return;
            }
            assemble_data =
                static_cast<uint8_t *>(heap_caps_malloc(static_cast<size_t>(event->payload_len),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (assemble_data == nullptr)
            {
                ++dropped_messages;
                return;
            }
            assemble_len    = static_cast<size_t>(event->payload_len);
            assemble_binary = binary;
        }
        if (assemble_data == nullptr
            || assemble_offset + static_cast<size_t>(event->data_len) > assemble_len)
        {
            return;
        }

        memcpy(assemble_data + assemble_offset,
               event->data_ptr,
               static_cast<size_t>(event->data_len));
        assemble_offset += static_cast<size_t>(event->data_len);
        if (assemble_offset == assemble_len)
        {
            transport_websocket_message_t message = {};
            message.data                          = assemble_data;
            message.len                           = assemble_len;
            message.binary                        = assemble_binary;
            if (xQueueSend(receive_queue, &message, 0) != pdTRUE)
            {
                free(message.data);
                ++dropped_messages;
            }
            assemble_data   = nullptr;
            assemble_len    = 0;
            assemble_offset = 0;
        }
    }

    /** @brief 释放尚未完成的分片消息 */
    void reset_assembly()
    {
        free(assemble_data);
        assemble_data   = nullptr;
        assemble_len    = 0;
        assemble_offset = 0;
    }

    esp_websocket_client_handle_t client            = nullptr; /**< ESP WebSocket Client */
    QueueHandle_t                 receive_queue     = nullptr; /**< 完整消息接收队列 */
    SemaphoreHandle_t             connection_signal = nullptr; /**< 连接结果信号量 */
    volatile bool                 connected         = false;   /**< 当前是否已连接 */
    volatile bool                 disconnected      = false;   /**< 是否收到断开或错误事件 */
    uint8_t                      *assemble_data     = nullptr; /**< 正在组装的消息数据 */
    size_t                        assemble_len      = 0;       /**< 正在组装的消息总长度 */
    size_t                        assemble_offset   = 0;       /**< 已组装长度 */
    bool                          assemble_binary   = false;   /**< 当前消息是否为二进制 */
    size_t                        max_message_bytes = 0;       /**< 单条消息最大长度 */
    uint32_t                      dropped_messages  = 0;       /**< 丢弃消息累计数 */
    bool                          events_registered = false;   /**< 是否需要注销事件回调 */
};

/**
 * @brief 校验底层发送长度
 *
 * @param[in] sent 实际发送长度
 * @param[in] expected 预期发送长度
 * @return ESP_OK 长度一致；ESP_FAIL 发送不完整
 */
static esp_err_t checked_send(int sent, size_t expected)
{
    return sent == static_cast<int>(expected) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 创建并连接 WebSocket
 *
 * @param[in] config WebSocket 配置
 * @param[out] out 成功时返回 WebSocket 对象
 * @return ESP_OK 已连接；其他值为参数、内存或连接错误码
 */
esp_err_t transport_websocket_open(const transport_websocket_config_t *config,
                                   transport_websocket_t             **out)
{
    if (config == nullptr || config->url == nullptr || out == nullptr
        || config->receive_queue_length == 0 || config->max_message_bytes == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out         = nullptr;

    auto *socket = new (std::nothrow) transport_websocket();
    if (socket == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = socket->open(*config);
    if (err != ESP_OK)
    {
        delete socket;
        return err;
    }
    *out = socket;
    return ESP_OK;
}

/**
 * @brief 发送文本消息
 *
 * @param[in] socket WebSocket 对象
 * @param[in] text 文本内容
 * @param[in] timeout_ms 超时时间，单位毫秒
 * @return ESP_OK 已完整发送；其他值为参数或发送错误码
 */
esp_err_t transport_websocket_send_text(transport_websocket_t *socket, const char *text,
                                        uint32_t timeout_ms)
{
    if (socket == nullptr || text == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t len = strlen(text);
    return checked_send(esp_websocket_client_send_text(socket->client,
                                                       text,
                                                       static_cast<int>(len),
                                                       pdMS_TO_TICKS(timeout_ms)),
                        len);
}

/**
 * @brief 发送二进制消息
 *
 * @param[in] socket WebSocket 对象
 * @param[in] data 二进制数据
 * @param[in] len 数据长度
 * @param[in] timeout_ms 超时时间，单位毫秒
 * @return ESP_OK 已完整发送；其他值为参数或发送错误码
 */
esp_err_t transport_websocket_send_binary(transport_websocket_t *socket, const uint8_t *data,
                                          size_t len, uint32_t timeout_ms)
{
    if (socket == nullptr || data == nullptr || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return checked_send(esp_websocket_client_send_bin(socket->client,
                                                      reinterpret_cast<const char *>(data),
                                                      static_cast<int>(len),
                                                      pdMS_TO_TICKS(timeout_ms)),
                        len);
}

/**
 * @brief 接收一条完整消息
 *
 * @param[in] socket WebSocket 对象
 * @param[out] message 接收到的消息，调用方使用后需释放
 * @param[in] timeout_ms 超时时间，单位毫秒
 * @return ESP_OK 已收到；ESP_ERR_TIMEOUT 超时；ESP_ERR_INVALID_STATE 已断开
 */
esp_err_t transport_websocket_receive(transport_websocket_t         *socket,
                                      transport_websocket_message_t *message, uint32_t timeout_ms)
{
    if (socket == nullptr || message == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(message, 0, sizeof(*message));
    if (xQueueReceive(socket->receive_queue, message, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        return ESP_OK;
    }
    return socket->disconnected ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}

/**
 * @brief 查询 WebSocket 是否已连接
 *
 * @param[in] socket WebSocket 对象
 * @return true 已连接；false 未连接或对象为空
 */
bool transport_websocket_is_connected(const transport_websocket_t *socket)
{
    return socket != nullptr && socket->connected;
}

/**
 * @brief 查询累计丢弃消息数
 *
 * @param[in] socket WebSocket 对象
 * @return 累计丢弃消息数；对象为空时返回 0
 */
uint32_t transport_websocket_dropped_messages(const transport_websocket_t *socket)
{
    return socket != nullptr ? socket->dropped_messages : 0;
}

/**
 * @brief 释放接收消息拥有的数据
 *
 * @param[in,out] message 待释放消息
 */
void transport_websocket_message_release(transport_websocket_message_t *message)
{
    if (message != nullptr)
    {
        free(message->data);
        memset(message, 0, sizeof(*message));
    }
}

/**
 * @brief 关闭并销毁 WebSocket 对象
 *
 * @param[in] socket WebSocket 对象
 */
void transport_websocket_close(transport_websocket_t *socket)
{
    delete socket;
}
