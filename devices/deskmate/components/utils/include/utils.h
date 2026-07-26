/**
 * @file utils.h
 * @brief 提供 Communication 共用的有界字符串与字节序工具
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 安全复制以空字符结尾的字符串
     *
     * 目标缓冲区有效时始终写入结尾空字符；source 为 NULL 时写入空字符串。
     *
     * @param[out] destination 目标缓冲区
     * @param[in] destination_size 目标缓冲区容量
     * @param[in] source 源字符串，可为 NULL
     */
    void utils_copy_string(char *destination, size_t destination_size, const char *source);

    /**
     * @brief 在指定偏移处安全追加字符串
     *
     * @param[in,out] destination 目标字符串缓冲区
     * @param[in] destination_size 目标缓冲区容量
     * @param[in,out] offset 当前末尾偏移，成功或截断后更新
     * @param[in] source 待追加字符串
     * @return true 完整追加；false 参数无效或内容被截断
     */
    bool utils_append_string(char *destination, size_t destination_size, size_t *offset, const char *source);

    /**
     * @brief 将十六进制字符转换为数值
     *
     * @param[in] character 十六进制字符
     * @return 0 至 15 表示成功；-1 表示字符无效
     */
    int utils_hex_digit_value(char character);

    /**
     * @brief 从大端字节序读取 16 位无符号整数
     *
     * @param[in] data 至少包含 2 字节的数据
     * @return 读取到的数值
     */
    uint16_t utils_read_be16(const uint8_t *data);

    /**
     * @brief 以大端字节序写入 16 位无符号整数
     *
     * @param[out] data 至少包含 2 字节的目标缓冲区
     * @param[in] value 待写入数值
     */
    void utils_write_be16(uint8_t *data, uint16_t value);

    /**
     * @brief 以大端字节序写入 32 位无符号整数
     *
     * @param[out] data 至少包含 4 字节的目标缓冲区
     * @param[in] value 待写入数值
     */
    void utils_write_be32(uint8_t *data, uint32_t value);

#ifdef __cplusplus
}
#endif
