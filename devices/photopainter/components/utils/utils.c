/**
 * @file utils.c
 * @brief 实现项目通用的字符串与字节序工具
 */
#include "utils.h"

uint8_t utils_gray2_pair_to_mono_byte(uint8_t first_gray2_byte, uint8_t second_gray2_byte)
{
    const uint8_t packed[2] = { first_gray2_byte, second_gray2_byte };
    uint8_t       output    = 0U;
    for (size_t source_index = 0U; source_index < 2U; ++source_index)
    {
        for (uint8_t pixel_index = 0U; pixel_index < 4U; ++pixel_index)
        {
            const uint8_t gray = (packed[source_index] >> (6U - (pixel_index * 2U))) & 0x03U;
            output             = (uint8_t) ((output << 1U) | (gray < 2U ? 1U : 0U));
        }
    }
    return output;
}

/**
 * @brief 安全复制以空字符结尾的字符串
 *
 * @param[out] destination 目标缓冲区
 * @param[in] destination_size 目标缓冲区容量
 * @param[in] source 源字符串，可为空指针
 */
void utils_copy_string(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0U)
    {
        return;
    }
    if (source == NULL)
    {
        destination[0] = '\0';
        return;
    }

    size_t index = 0U;
    while (index + 1U < destination_size && source[index] != '\0')
    {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

/**
 * @brief 向字符串缓冲区的指定偏移安全追加字符串
 *
 * @param[in,out] destination 目标字符串缓冲区
 * @param[in] destination_size 目标缓冲区容量
 * @param[in,out] offset 当前末尾偏移
 * @param[in] source 待追加字符串
 * @return true 完整追加，false 参数无效或内容被截断
 */
bool utils_append_string(char *destination, size_t destination_size, size_t *offset,
                         const char *source)
{
    if (destination == NULL || destination_size == 0U || offset == NULL
        || *offset >= destination_size || source == NULL)
    {
        return false;
    }

    size_t source_index = 0U;
    while (*offset + 1U < destination_size && source[source_index] != '\0')
    {
        destination[*offset] = source[source_index];
        ++(*offset);
        ++source_index;
    }
    destination[*offset] = '\0';
    return source[source_index] == '\0';
}

/**
 * @brief 从小端字节序读取 16 位无符号整数
 *
 * @param[in] data 至少包含 2 字节的数据指针
 * @return 读取到的 16 位无符号整数
 */
uint16_t utils_read_le16(const uint8_t *data)
{
    return (uint16_t) data[0] | ((uint16_t) data[1] << 8U);
}

/**
 * @brief 从小端字节序读取 32 位无符号整数
 *
 * @param[in] data 至少包含 4 字节的数据指针
 * @return 读取到的 32 位无符号整数
 */
uint32_t utils_read_le32(const uint8_t *data)
{
    return (uint32_t) data[0] | ((uint32_t) data[1] << 8U) | ((uint32_t) data[2] << 16U)
           | ((uint32_t) data[3] << 24U);
}

/**
 * @brief 从小端字节序读取 64 位无符号整数
 *
 * @param[in] data 至少包含 8 字节的数据指针
 * @return 读取到的 64 位无符号整数
 */
uint64_t utils_read_le64(const uint8_t *data)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < 8U; ++index)
    {
        value |= (uint64_t) data[index] << (index * 8U);
    }
    return value;
}

/**
 * @brief 从大端字节序读取 16 位无符号整数
 *
 * @param[in] data 至少包含 2 字节的数据指针
 * @return 读取到的 16 位无符号整数
 */
uint16_t utils_read_be16(const uint8_t *data)
{
    return ((uint16_t) data[0] << 8U) | (uint16_t) data[1];
}

/**
 * @brief 从大端字节序读取 32 位无符号整数
 *
 * @param[in] data 至少包含 4 字节的数据指针
 * @return 读取到的 32 位无符号整数
 */
uint32_t utils_read_be32(const uint8_t *data)
{
    return ((uint32_t) data[0] << 24U) | ((uint32_t) data[1] << 16U) | ((uint32_t) data[2] << 8U)
           | (uint32_t) data[3];
}

/**
 * @brief 从大端字节序读取 64 位无符号整数
 *
 * @param[in] data 至少包含 8 字节的数据指针
 * @return 读取到的 64 位无符号整数
 */
uint64_t utils_read_be64(const uint8_t *data)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < 8U; ++index)
    {
        value = (value << 8U) | data[index];
    }
    return value;
}

/**
 * @brief 以小端字节序写入 16 位无符号整数
 *
 * @param[out] data 至少包含 2 字节的目标指针
 * @param[in] value 待写入数值
 */
void utils_write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t) value;
    data[1] = (uint8_t) (value >> 8U);
}

/**
 * @brief 以小端字节序写入 32 位无符号整数
 *
 * @param[out] data 至少包含 4 字节的目标指针
 * @param[in] value 待写入数值
 */
void utils_write_le32(uint8_t *data, uint32_t value)
{
    for (size_t index = 0U; index < 4U; ++index)
    {
        data[index] = (uint8_t) (value >> (index * 8U));
    }
}

/**
 * @brief 以小端字节序写入 64 位无符号整数
 *
 * @param[out] data 至少包含 8 字节的目标指针
 * @param[in] value 待写入数值
 */
void utils_write_le64(uint8_t *data, uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index)
    {
        data[index] = (uint8_t) (value >> (index * 8U));
    }
}

/**
 * @brief 以大端字节序写入 16 位无符号整数
 *
 * @param[out] data 至少包含 2 字节的目标指针
 * @param[in] value 待写入数值
 */
void utils_write_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t) (value >> 8U);
    data[1] = (uint8_t) value;
}

/**
 * @brief 以大端字节序写入 32 位无符号整数
 *
 * @param[out] data 至少包含 4 字节的目标指针
 * @param[in] value 待写入数值
 */
void utils_write_be32(uint8_t *data, uint32_t value)
{
    for (size_t index = 0U; index < 4U; ++index)
    {
        data[index] = (uint8_t) (value >> ((3U - index) * 8U));
    }
}

/**
 * @brief 以大端字节序写入 64 位无符号整数
 *
 * @param[out] data 至少包含 8 字节的目标指针
 * @param[in] value 待写入数值
 */
void utils_write_be64(uint8_t *data, uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index)
    {
        data[index] = (uint8_t) (value >> ((7U - index) * 8U));
    }
}

/**
 * @brief 将十六进制字符转换为数值
 *
 * @param[in] character 十六进制字符
 * @return 0 到 15 表示成功，-1 表示字符无效
 */
int utils_hex_digit_value(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

/**
 * @brief 判断字符串是否为指定长度的十六进制文本
 *
 * @param[in] text 待检查字符串
 * @param[in] length 期望字符数
 * @return true 字符串合法，false 字符串不合法
 */
bool utils_is_hex_string(const char *text, size_t length)
{
    if (text == NULL)
    {
        return false;
    }
    for (size_t index = 0U; index < length; ++index)
    {
        if (text[index] == '\0' || utils_hex_digit_value(text[index]) < 0)
        {
            return false;
        }
    }
    return text[length] == '\0';
}

/**
 * @brief 将字节数组编码为以空字符结尾的十六进制字符串
 *
 * @param[out] destination 目标字符串缓冲区
 * @param[in] destination_size 目标缓冲区容量
 * @param[in] data 源字节数组
 * @param[in] length 源字节数
 * @param[in] uppercase true 使用大写字母，false 使用小写字母
 * @return true 编码成功，false 参数或缓冲区容量无效
 */
bool utils_bytes_to_hex(char *destination, size_t destination_size, const uint8_t *data,
                        size_t length, bool uppercase)
{
    if (destination == NULL || (data == NULL && length != 0U) || length > (SIZE_MAX - 1U) / 2U
        || destination_size < length * 2U + 1U)
    {
        return false;
    }

    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t index = 0U; index < length; ++index)
    {
        destination[index * 2U]      = digits[data[index] >> 4U];
        destination[index * 2U + 1U] = digits[data[index] & 0x0FU];
    }
    destination[length * 2U] = '\0';
    return true;
}

/**
 * @brief 将十六进制字符串解码为字节数组
 *
 * @param[out] destination 目标字节缓冲区
 * @param[in] destination_size 目标缓冲区容量
 * @param[in] text 源十六进制字符串
 * @param[in] text_length 源字符数
 * @return true 解码成功，false 参数、容量或字符无效
 */
bool utils_hex_to_bytes(uint8_t *destination, size_t destination_size, const char *text,
                        size_t text_length)
{
    if (text == NULL || (destination == NULL && text_length != 0U) || (text_length & 1U) != 0U
        || destination_size < text_length / 2U)
    {
        return false;
    }

    for (size_t index = 0U; index < text_length; index += 2U)
    {
        const int high = utils_hex_digit_value(text[index]);
        const int low  = utils_hex_digit_value(text[index + 1U]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        destination[index / 2U] = (uint8_t) ((high << 4) | low);
    }
    return true;
}
