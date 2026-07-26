/**
 * @file utils.c
 * @brief 实现 Communication 共用的有界字符串与字节序工具
 */
#include "utils.h"

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

bool utils_append_string(char *destination, size_t destination_size, size_t *offset, const char *source)
{
    if (destination == NULL || destination_size == 0U || offset == NULL || *offset >= destination_size
        || source == NULL)
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

uint16_t utils_read_be16(const uint8_t *data)
{
    return ((uint16_t) data[0] << 8U) | (uint16_t) data[1];
}

void utils_write_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t) (value >> 8U);
    data[1] = (uint8_t) value;
}

void utils_write_be32(uint8_t *data, uint32_t value)
{
    for (size_t index = 0U; index < 4U; ++index)
    {
        data[index] = (uint8_t) (value >> ((3U - index) * 8U));
    }
}
