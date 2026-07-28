/**
 * @file web_file_service_path.cpp
 * @brief 网页文件服务的路径、JSON 与响应头编码安全实现
 */
#include "web_file_service_internal.hpp"

#include <stdint.h>
#include <string.h>

#include "system_filesystem.h"

#define WEB_FILE_PATH_SEGMENT_MAX_BYTES 255U

static int web_file_hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9')
    {
        return (int) (value - '0');
    }
    if (value >= 'a' && value <= 'f')
    {
        return (int) (value - 'a') + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return (int) (value - 'A') + 10;
    }
    return -1;
}

/**
 * @brief 验证字节序列仅包含最短形式的 Unicode 标量
 *
 * 本函数逐个恢复 Unicode 标量，拒绝过长编码、孤立续字节、代理项、超出 U+10FFFF 的
 * 标量及 C0/C1 控制字符。调用方已经单独拒绝 NUL、反斜杠等路径专用禁用字节。
 *
 * @param[in] input 待验证字节序列
 * @param[in] input_size 序列长度
 * @return true 序列有效；false 序列无效
 */
static bool web_file_utf8_is_valid(const uint8_t *input, size_t input_size)
{
    size_t offset = 0U;

    while (offset < input_size)
    {
        const uint8_t first = input[offset];
        uint32_t      scalar;
        size_t        sequence_size;

        if (first <= 0x7FU)
        {
            scalar        = first;
            sequence_size = 1U;
        }
        else if (first >= 0xC2U && first <= 0xDFU)
        {
            if (offset + 1U >= input_size || (input[offset + 1U] & 0xC0U) != 0x80U)
            {
                return false;
            }
            scalar        = ((uint32_t) (first & 0x1FU) << 6U) | (uint32_t) (input[offset + 1U] & 0x3FU);
            sequence_size = 2U;
        }
        else if (first >= 0xE0U && first <= 0xEFU)
        {
            if (offset + 2U >= input_size || (input[offset + 1U] & 0xC0U) != 0x80U
                || (input[offset + 2U] & 0xC0U) != 0x80U)
            {
                return false;
            }
            const uint8_t second = input[offset + 1U];
            if ((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second > 0x9FU))
            {
                return false;
            }
            scalar        = ((uint32_t) (first & 0x0FU) << 12U) | ((uint32_t) (second & 0x3FU) << 6U)
                            | (uint32_t) (input[offset + 2U] & 0x3FU);
            sequence_size = 3U;
        }
        else if (first >= 0xF0U && first <= 0xF4U)
        {
            if (offset + 3U >= input_size || (input[offset + 1U] & 0xC0U) != 0x80U
                || (input[offset + 2U] & 0xC0U) != 0x80U || (input[offset + 3U] & 0xC0U) != 0x80U)
            {
                return false;
            }
            const uint8_t second = input[offset + 1U];
            if ((first == 0xF0U && second < 0x90U) || (first == 0xF4U && second > 0x8FU))
            {
                return false;
            }
            scalar        = ((uint32_t) (first & 0x07U) << 18U) | ((uint32_t) (second & 0x3FU) << 12U)
                            | ((uint32_t) (input[offset + 2U] & 0x3FU) << 6U) | (uint32_t) (input[offset + 3U] & 0x3FU);
            sequence_size = 4U;
        }
        else
        {
            return false;
        }

        if (scalar <= 0x1FU || (scalar >= 0x7FU && scalar <= 0x9FU) || (scalar >= 0xD800U && scalar <= 0xDFFFU)
            || scalar > 0x10FFFFU)
        {
            return false;
        }
        offset += sequence_size;
    }
    return true;
}

/**
 * @brief 按 ASCII 大小写不敏感语义比较固定长度字节序列
 *
 * 仅将 `A` 到 `Z` 折叠为对应小写字母，所有非 ASCII 字节保持原值，避免区域设置改变路径
 * 判定语义。
 *
 * @param[in] left 左侧字节序列
 * @param[in] right 右侧字节序列
 * @param[in] size 比较长度
 * @return true 两个序列按 ASCII 大小写不敏感语义相等；false 不相等
 */
static bool web_file_ascii_case_equal(const char *left, const char *right, size_t size)
{
    for (size_t offset = 0U; offset < size; ++offset)
    {
        unsigned char left_value  = (unsigned char) left[offset];
        unsigned char right_value = (unsigned char) right[offset];
        if (left_value >= 'A' && left_value <= 'Z')
        {
            left_value = (unsigned char) (left_value + ('a' - 'A'));
        }
        if (right_value >= 'A' && right_value <= 'Z')
        {
            right_value = (unsigned char) (right_value + ('a' - 'A'));
        }
        if (left_value != right_value)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 校验解码后路径的段边界与保留名称
 *
 * 根路径是唯一允许没有文件名段或以斜杠结束的路径。其他路径的每段按解码后的 UTF-8
 * 字节数限制为 255，并拒绝可改变目录解析语义的特殊段。
 *
 * @param[in] path 解码后的逻辑路径
 * @param[in] path_size 不含 NUL 的路径长度
 * @return ESP_OK 有效；ESP_ERR_INVALID_ARG 段语义无效；
 *         ESP_ERR_INVALID_SIZE 段超过长度限制
 */
static esp_err_t web_file_validate_path_segments(const char *path, size_t path_size)
{
    if (path_size == 1U)
    {
        return ESP_OK;
    }

    size_t segment_start = 1U;
    size_t segment_index = 0U;
    for (size_t offset = 1U; offset <= path_size; ++offset)
    {
        if (offset != path_size && path[offset] != '/')
        {
            continue;
        }

        const size_t segment_size = offset - segment_start;
        if (segment_size == 0U)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (segment_size > WEB_FILE_PATH_SEGMENT_MAX_BYTES)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((segment_size == 1U && path[segment_start] == '.')
            || (segment_size == 2U && path[segment_start] == '.' && path[segment_start + 1U] == '.'))
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (segment_index == 0U && segment_size == sizeof(".deskmate-web") - 1U
            && web_file_ascii_case_equal(path + segment_start, ".deskmate-web", segment_size))
        {
            return ESP_ERR_INVALID_ARG;
        }

        ++segment_index;
        segment_start = offset + 1U;
    }
    return ESP_OK;
}

esp_err_t web_file_path_decode_and_map(const char *encoded, char *logical, size_t logical_size, char *filesystem,
                                       size_t filesystem_size)
{
    if (encoded == NULL || logical == NULL || filesystem == NULL || logical_size == 0U || filesystem_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char   normalized[WEB_FILE_LOGICAL_PATH_BUFFER_SIZE];
    size_t encoded_offset = 0U;
    size_t decoded_size   = 0U;

    while (encoded[encoded_offset] != '\0')
    {
        unsigned char decoded;
        if (encoded[encoded_offset] == '%')
        {
            const unsigned char high = (unsigned char) encoded[encoded_offset + 1U];
            if (high == '\0')
            {
                return ESP_ERR_INVALID_ARG;
            }
            const unsigned char low = (unsigned char) encoded[encoded_offset + 2U];
            if (low == '\0')
            {
                return ESP_ERR_INVALID_ARG;
            }
            const int high_value = web_file_hex_value(high);
            const int low_value  = web_file_hex_value(low);
            if (high_value < 0 || low_value < 0)
            {
                return ESP_ERR_INVALID_ARG;
            }
            decoded = (unsigned char) ((high_value << 4) | low_value);
            encoded_offset += 3U;
        }
        else
        {
            decoded = (unsigned char) encoded[encoded_offset];
            ++encoded_offset;
        }

        if (decoded == '\0' || decoded == '\\' || decoded <= 0x1FU || decoded == 0x7FU)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (decoded_size >= sizeof(normalized) - 1U)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        normalized[decoded_size] = (char) decoded;
        ++decoded_size;
    }
    normalized[decoded_size] = '\0';

    if (decoded_size == 0U || normalized[0] != '/'
        || !web_file_utf8_is_valid((const uint8_t *) normalized, decoded_size))
    {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t segment_error = web_file_validate_path_segments(normalized, decoded_size);
    if (segment_error != ESP_OK)
    {
        return segment_error;
    }

    const size_t mount_size           = sizeof(SYSTEM_FILESYSTEM_MOUNT_POINT) - 1U;
    const size_t filesystem_path_size = mount_size + (decoded_size == 1U ? 0U : decoded_size);
    if (logical_size < decoded_size + 1U || filesystem_size < filesystem_path_size + 1U)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(logical, normalized, decoded_size + 1U);
    memcpy(filesystem, SYSTEM_FILESYSTEM_MOUNT_POINT, mount_size);
    if (decoded_size == 1U)
    {
        filesystem[mount_size] = '\0';
    }
    else
    {
        memcpy(filesystem + mount_size, normalized, decoded_size + 1U);
    }
    return ESP_OK;
}

esp_err_t web_file_path_map_logical(const char *logical, char *filesystem, size_t filesystem_size)
{
    if (logical == NULL || filesystem == NULL || filesystem_size == 0U || logical[0] != '/')
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t logical_size = strnlen(logical, WEB_FILE_LOGICAL_PATH_BUFFER_SIZE);
    if (logical_size == 0U || logical_size >= WEB_FILE_LOGICAL_PATH_BUFFER_SIZE
        || !web_file_utf8_is_valid((const uint8_t *) logical, logical_size))
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t offset = 0U; offset < logical_size; ++offset)
    {
        const unsigned char value = (unsigned char) logical[offset];
        if (value == '\\' || value <= 0x1FU || value == 0x7FU)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }

    const esp_err_t segment_error = web_file_validate_path_segments(logical, logical_size);
    if (segment_error != ESP_OK)
    {
        return segment_error;
    }

    const size_t mount_size           = sizeof(SYSTEM_FILESYSTEM_MOUNT_POINT) - 1U;
    const size_t filesystem_path_size = mount_size + (logical_size == 1U ? 0U : logical_size);
    if (filesystem_size < filesystem_path_size + 1U)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(filesystem, SYSTEM_FILESYSTEM_MOUNT_POINT, mount_size);
    if (logical_size == 1U)
    {
        filesystem[mount_size] = '\0';
    }
    else
    {
        memcpy(filesystem + mount_size, logical, logical_size + 1U);
    }
    return ESP_OK;
}

static size_t web_file_json_escape_size(unsigned char value)
{
    switch (value)
    {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            return 2U;
        default:
            return value < 0x20U ? 6U : 1U;
    }
}

esp_err_t web_file_json_escape(const char *input, char *output, size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t required_size = 1U;
    for (const unsigned char *cursor = (const unsigned char *) input; *cursor != '\0'; ++cursor)
    {
        const size_t escaped_size = web_file_json_escape_size(*cursor);
        if (required_size > SIZE_MAX - escaped_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        required_size += escaped_size;
    }
    if (output_size < required_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    static const char hex[]         = "0123456789ABCDEF";
    size_t            output_offset = 0U;
    for (const unsigned char *cursor = (const unsigned char *) input; *cursor != '\0'; ++cursor)
    {
        switch (*cursor)
        {
            case '"':
                output[output_offset++] = '\\';
                output[output_offset++] = '"';
                break;
            case '\\':
                output[output_offset++] = '\\';
                output[output_offset++] = '\\';
                break;
            case '\b':
                output[output_offset++] = '\\';
                output[output_offset++] = 'b';
                break;
            case '\f':
                output[output_offset++] = '\\';
                output[output_offset++] = 'f';
                break;
            case '\n':
                output[output_offset++] = '\\';
                output[output_offset++] = 'n';
                break;
            case '\r':
                output[output_offset++] = '\\';
                output[output_offset++] = 'r';
                break;
            case '\t':
                output[output_offset++] = '\\';
                output[output_offset++] = 't';
                break;
            default:
                if (*cursor < 0x20U)
                {
                    output[output_offset++] = '\\';
                    output[output_offset++] = 'u';
                    output[output_offset++] = '0';
                    output[output_offset++] = '0';
                    output[output_offset++] = hex[*cursor >> 4U];
                    output[output_offset++] = hex[*cursor & 0x0FU];
                }
                else
                {
                    output[output_offset++] = (char) *cursor;
                }
                break;
        }
    }
    output[output_offset] = '\0';
    return ESP_OK;
}

static bool web_file_percent_byte_is_safe(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')
           || value == '-' || value == '.' || value == '_' || value == '~';
}

esp_err_t web_file_percent_encode(const char *input, char *output, size_t output_size)
{
    if (input == NULL || output == NULL || output_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t required_size = 1U;
    for (const unsigned char *cursor = (const unsigned char *) input; *cursor != '\0'; ++cursor)
    {
        const size_t encoded_size = web_file_percent_byte_is_safe(*cursor) ? 1U : 3U;
        if (required_size > SIZE_MAX - encoded_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        required_size += encoded_size;
    }
    if (output_size < required_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    static const char hex[]         = "0123456789ABCDEF";
    size_t            output_offset = 0U;
    for (const unsigned char *cursor = (const unsigned char *) input; *cursor != '\0'; ++cursor)
    {
        if (web_file_percent_byte_is_safe(*cursor))
        {
            output[output_offset++] = (char) *cursor;
        }
        else
        {
            output[output_offset++] = '%';
            output[output_offset++] = hex[*cursor >> 4U];
            output[output_offset++] = hex[*cursor & 0x0FU];
        }
    }
    output[output_offset] = '\0';
    return ESP_OK;
}
