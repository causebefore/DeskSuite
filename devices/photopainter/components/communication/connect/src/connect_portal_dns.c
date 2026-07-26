/*
 * 文件职责：实现配网 Portal 的 DNS 劫持 socket 与报文构造。
 * 主要依赖：ESP-IDF/LwIP socket、FreeRTOS 临界区。
 * 调用方：connect_portal_dns_task.c。
 */
#include "connect_internal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "utils.h"

#define CONNECT_PORTAL_IP              "192.168.4.1"
#define CONNECT_PORTAL_DNS_PORT        53
#define CONNECT_PORTAL_DNS_BUFFER_SIZE 512
#define CONNECT_PORTAL_DNS_HEADER_SIZE 12

static const char *TAG = "connect_portal_dns";

static int          s_dns_sock = -1;
static portMUX_TYPE s_dns_lock = portMUX_INITIALIZER_UNLOCKED;

static size_t dns_find_question_end(const uint8_t *packet, size_t length)
{
    size_t offset = CONNECT_PORTAL_DNS_HEADER_SIZE;

    while (offset < length)
    {
        const uint8_t label_len = packet[offset++];
        if (label_len == 0)
        {
            break;
        }
        if ((label_len & 0xC0) == 0xC0)
        {
            if (offset >= length)
            {
                return 0;
            }
            offset++;
            break;
        }
        if (label_len > 63 || offset + label_len > length)
        {
            return 0;
        }
        offset += label_len;
    }

    if (offset + 4 > length)
    {
        return 0;
    }
    return offset + 4;
}

static int dns_build_portal_response(uint8_t *packet, int request_len)
{
    static const uint8_t portal_ip[4] = { 192, 168, 4, 1 };

    if (request_len < CONNECT_PORTAL_DNS_HEADER_SIZE)
    {
        return -1;
    }

    const uint16_t question_count = utils_read_be16(&packet[4]);
    if (question_count == 0)
    {
        return -1;
    }

    const size_t question_end = dns_find_question_end(packet, (size_t) request_len);
    if (question_end == 0 || question_end + 16 > CONNECT_PORTAL_DNS_BUFFER_SIZE)
    {
        return -1;
    }

    packet[2] = 0x81;
    packet[3] = 0x80;
    utils_write_be16(&packet[4], 1U);
    utils_write_be16(&packet[6], 1U);
    utils_write_be16(&packet[8], 0U);
    utils_write_be16(&packet[10], 0U);

    uint8_t *answer = &packet[question_end];
    answer[0]       = 0xC0;
    answer[1]       = 0x0C;
    utils_write_be16(&answer[2], 1U);
    utils_write_be16(&answer[4], 1U);
    utils_write_be32(&answer[6], 60U);
    utils_write_be16(&answer[10], sizeof(portal_ip));
    memcpy(&answer[12], portal_ip, sizeof(portal_ip));

    return (int) question_end + 16;
}

/**
 * @brief 打开并绑定配网 DNS UDP socket
 */
esp_err_t connect_internal_portal_dns_open(void)
{
    struct sockaddr_in listen_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONNECT_PORTAL_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    taskENTER_CRITICAL(&s_dns_lock);
    const bool already_open = s_dns_sock >= 0;
    taskEXIT_CRITICAL(&s_dns_lock);
    if (already_open)
    {
        return ESP_OK;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGW(TAG, "创建 DNS socket 失败: errno=%d", errno);
        return ESP_FAIL;
    }

    const int reuse = 1;
    (void) setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const struct timeval receive_timeout = {
        .tv_sec  = 0,
        .tv_usec = 200000,
    };
    (void) setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));

    if (bind(sock, (const struct sockaddr *) &listen_addr, sizeof(listen_addr)) < 0)
    {
        ESP_LOGW(TAG, "绑定 DNS 端口失败: errno=%d", errno);
        close(sock);
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_dns_lock);
    s_dns_sock = sock;
    taskEXIT_CRITICAL(&s_dns_lock);
    ESP_LOGI(TAG, "配网 DNS 已启动，所有域名解析到 " CONNECT_PORTAL_IP);
    return ESP_OK;
}

/**
 * @brief 阻塞处理一个配网 DNS 请求
 */
esp_err_t connect_internal_portal_dns_process_once(void)
{
    taskENTER_CRITICAL(&s_dns_lock);
    const int sock = s_dns_sock;
    taskEXIT_CRITICAL(&s_dns_lock);
    if (sock < 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t            packet[CONNECT_PORTAL_DNS_BUFFER_SIZE];
    struct sockaddr_in source_addr = { 0 };
    socklen_t          source_len  = sizeof(source_addr);
    const int          len =
        recvfrom(sock, packet, sizeof(packet), 0, (struct sockaddr *) &source_addr, &source_len);
    if (len < 0)
    {
        taskENTER_CRITICAL(&s_dns_lock);
        const bool still_open = s_dns_sock == sock;
        taskEXIT_CRITICAL(&s_dns_lock);
        if (still_open && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return ESP_OK;
        }
        if (still_open)
        {
            ESP_LOGW(TAG, "接收 DNS 请求失败: errno=%d", errno);
        }
        return still_open ? ESP_FAIL : ESP_ERR_INVALID_STATE;
    }

    const int response_len = dns_build_portal_response(packet, len);
    if (response_len <= 0)
    {
        return ESP_OK;
    }
    if (sendto(sock,
               packet,
               (size_t) response_len,
               0,
               (const struct sockaddr *) &source_addr,
               source_len)
        < 0)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 关闭配网 DNS socket 并中断阻塞接收
 */
void connect_internal_portal_dns_close(void)
{
    taskENTER_CRITICAL(&s_dns_lock);
    const int sock = s_dns_sock;
    s_dns_sock     = -1;
    taskEXIT_CRITICAL(&s_dns_lock);
    if (sock >= 0)
    {
        close(sock);
    }
}
