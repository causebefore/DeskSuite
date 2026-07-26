/**
 * @file display_frame_protocol.c
 * @brief 实现 PPF2/GRAY2 显示帧的完整性校验
 */
#include "display_frame_protocol.h"

#include <string.h>

#include "esp_log.h"
#include "utils.h"

/** @brief 日志标签 */
static const char *TAG = "display_frame";

/**
 * @brief 把 Manifest 日期时间版本转换为 PPF2 头内的十进制整数
 *
 * @param[in] version 格式为 YYYYMMDD-HHMMSS 的版本字符串
 * @param[out] out_value 转换后的十四位十进制整数
 * @return true 转换成功；false 格式无效
 */
static bool display_frame_protocol_parse_version(const char *version, uint64_t *out_value)
{
    if (version == NULL || out_value == NULL || strlen(version) != 15U || version[8] != '-')
    {
        return false;
    }

    uint64_t value = 0U;
    for (size_t index = 0U; index < 15U; ++index)
    {
        if (index == 8U)
        {
            continue;
        }
        if (version[index] < '0' || version[index] > '9')
        {
            return false;
        }
        value = value * 10U + (uint64_t) (version[index] - '0');
    }
    *out_value = value;
    return true;
}

/**
 * @brief 计算摘要并与 Manifest 小写十六进制摘要比较
 *
 * @param[in] data 待计算数据
 * @param[in] size_bytes 数据长度
 * @param[in] expected_hex 期望的 64 字符十六进制摘要
 * @return ESP_OK 匹配；ESP_ERR_INVALID_RESPONSE 摘要格式或内容不匹配；或计算错误码
 */
static esp_err_t display_frame_protocol_check_sha256(const uint8_t *data, size_t size_bytes,
                                                     const char *expected_hex)
{
    uint8_t digest[UTILS_SHA256_DIGEST_SIZE];
    uint8_t expected[UTILS_SHA256_DIGEST_SIZE];
    if (!utils_hex_to_bytes(expected, sizeof(expected), expected_hex, 64U))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const esp_err_t error = utils_sha256(data, size_bytes, digest);
    if (error != ESP_OK)
    {
        return error;
    }
    return memcmp(digest, expected, sizeof(digest)) == 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t display_frame_protocol_validate_borrow(
    const uint8_t *frame_data, size_t frame_size, const display_protocol_page_t *expected_page,
    display_frame_protocol_view_t *out_view)
{
    if (frame_data == NULL || expected_page == NULL || out_view == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_view, 0, sizeof(*out_view));
    if (frame_size != DISPLAY_FRAME_PROTOCOL_FILE_SIZE
        || expected_page->file_size != DISPLAY_FRAME_PROTOCOL_FILE_SIZE)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t expected_version;
    if (memcmp(frame_data, DISPLAY_FRAME_PROTOCOL_FORMAT, 4U) != 0
        || frame_data[4] != DISPLAY_FRAME_PROTOCOL_VERSION
        || frame_data[5] != DISPLAY_FRAME_PROTOCOL_BITS_PER_PIXEL
        || utils_read_le16(frame_data + 6U) != DISPLAY_FRAME_PROTOCOL_HEADER_SIZE
        || utils_read_le16(frame_data + 8U) != DISPLAY_FRAME_PROTOCOL_WIDTH
        || utils_read_le16(frame_data + 10U) != DISPLAY_FRAME_PROTOCOL_HEIGHT
        || utils_read_le32(frame_data + 12U) != DISPLAY_FRAME_PROTOCOL_PAYLOAD_SIZE
        || !display_frame_protocol_parse_version(expected_page->content_version, &expected_version)
        || utils_read_le64(frame_data + 20U) != expected_version
        || utils_read_le32(frame_data + 28U) != 0U)
    {
        ESP_LOGE(TAG, "PPF2 帧头字段无效");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t *payload = frame_data + DISPLAY_FRAME_PROTOCOL_HEADER_SIZE;
    const uint32_t payload_crc = utils_crc32_ieee(payload, DISPLAY_FRAME_PROTOCOL_PAYLOAD_SIZE);
    if (utils_read_le32(frame_data + 16U) != payload_crc || expected_page->crc32 != payload_crc)
    {
        ESP_LOGE(TAG, "PPF2 像素 CRC32 校验失败");
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t error = display_frame_protocol_check_sha256(
        payload, DISPLAY_FRAME_PROTOCOL_PAYLOAD_SIZE, expected_page->payload_sha256);
    if (error == ESP_OK)
    {
        error = display_frame_protocol_check_sha256(frame_data, frame_size, expected_page->sha256);
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "PPF2 SHA-256 校验失败: %s", esp_err_to_name(error));
        return error;
    }

    *out_view = (display_frame_protocol_view_t){
        .pixels       = payload,
        .size_bytes   = DISPLAY_FRAME_PROTOCOL_PAYLOAD_SIZE,
        .stride_bytes = DISPLAY_FRAME_PROTOCOL_STRIDE,
        .width        = DISPLAY_FRAME_PROTOCOL_WIDTH,
        .height       = DISPLAY_FRAME_PROTOCOL_HEIGHT,
    };
    return ESP_OK;
}
