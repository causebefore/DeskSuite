/**
 * @file utils.h
 * @brief 声明项目通用的字符串、字节序与校验工具
 */
#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief IEEE CRC-32 初始值 */
#define UTILS_CRC32_IEEE_INITIAL_VALUE        0xFFFFFFFFU
/** @brief IEEE CRC-32 结果异或值 */
#define UTILS_CRC32_IEEE_FINAL_XOR            0xFFFFFFFFU
/** @brief CRC-16/CCITT-FALSE 初始值 */
#define UTILS_CRC16_CCITT_FALSE_INITIAL_VALUE 0xFFFFU
/** @brief CRC-16/MODBUS 初始值 */
#define UTILS_CRC16_MODBUS_INITIAL_VALUE      0xFFFFU
/** @brief SHA-256 摘要字节数 */
#define UTILS_SHA256_DIGEST_SIZE              32U

    /** @brief SHA-256 流式计算上下文的非公开类型 */
    typedef struct utils_sha256_context utils_sha256_context_t;

    /**
 * @brief 安全复制以空字符结尾的字符串
 *
 * 目标缓冲区有效且容量大于零时，结果始终以空字符结尾。源字符串为空指针时，
 * 目标缓冲区会被写为空字符串。
 *
 * @param[out] destination 目标缓冲区
 * @param[in] destination_size 目标缓冲区容量
 * @param[in] source 源字符串，可为空指针
 */
    void utils_copy_string(char *destination, size_t destination_size, const char *source);

    /**
 * @brief 向字符串缓冲区的指定偏移安全追加字符串
 *
 * @param[in,out] destination 目标字符串缓冲区
 * @param[in] destination_size 目标缓冲区容量
 * @param[in,out] offset 当前末尾偏移，返回时更新为新的末尾偏移
 * @param[in] source 待追加字符串
 * @return true 完整追加，false 参数无效或内容被截断
 */
    bool utils_append_string(char *destination, size_t destination_size, size_t *offset,
                             const char *source);

    /**
     * @brief 判断一段单调时间是否达到指定持续时间
     *
     * 起点必须大于零，终点不得早于起点。函数不读取时钟，适合按键等纯策略边界测试。
     *
     * @param[in] started_at_us 起始单调时间，微秒
     * @param[in] ended_at_us 结束单调时间，微秒
     * @param[in] required_us 要求的最短持续时间，必须大于零
     * @return true 持续时间达到要求；false 输入无效或尚未达到
     */
    static inline bool utils_duration_reached_us(int64_t started_at_us,
                                                 int64_t ended_at_us,
                                                 int64_t required_us)
    {
        return started_at_us > 0 && required_us > 0 && ended_at_us >= started_at_us
               && ended_at_us - started_at_us >= required_us;
    }

    /**
 * @brief 从小端字节序读取 16 位无符号整数
 *
 * @param[in] data 至少包含 2 字节的数据指针
 * @return 读取到的 16 位无符号整数
 */
    uint16_t utils_read_le16(const uint8_t *data);

    /**
 * @brief 从小端字节序读取 32 位无符号整数
 *
 * @param[in] data 至少包含 4 字节的数据指针
 * @return 读取到的 32 位无符号整数
 */
    uint32_t utils_read_le32(const uint8_t *data);

    /**
 * @brief 从小端字节序读取 64 位无符号整数
 *
 * @param[in] data 至少包含 8 字节的数据指针
 * @return 读取到的 64 位无符号整数
 */
    uint64_t utils_read_le64(const uint8_t *data);

    /**
 * @brief 从大端字节序读取 16 位无符号整数
 *
 * @param[in] data 至少包含 2 字节的数据指针
 * @return 读取到的 16 位无符号整数
 */
    uint16_t utils_read_be16(const uint8_t *data);

    /**
 * @brief 从大端字节序读取 32 位无符号整数
 *
 * @param[in] data 至少包含 4 字节的数据指针
 * @return 读取到的 32 位无符号整数
 */
    uint32_t utils_read_be32(const uint8_t *data);

    /**
 * @brief 从大端字节序读取 64 位无符号整数
 *
 * @param[in] data 至少包含 8 字节的数据指针
 * @return 读取到的 64 位无符号整数
 */
    uint64_t utils_read_be64(const uint8_t *data);

    /**
 * @brief 以小端字节序写入 16 位无符号整数
 *
 * @param[out] data 至少包含 2 字节的目标指针
 * @param[in] value 待写入数值
 */
    void utils_write_le16(uint8_t *data, uint16_t value);

    /**
 * @brief 以小端字节序写入 32 位无符号整数
 *
 * @param[out] data 至少包含 4 字节的目标指针
 * @param[in] value 待写入数值
 */
    void utils_write_le32(uint8_t *data, uint32_t value);

    /**
 * @brief 以小端字节序写入 64 位无符号整数
 *
 * @param[out] data 至少包含 8 字节的目标指针
 * @param[in] value 待写入数值
 */
    void utils_write_le64(uint8_t *data, uint64_t value);

    /**
 * @brief 以大端字节序写入 16 位无符号整数
 *
 * @param[out] data 至少包含 2 字节的目标指针
 * @param[in] value 待写入数值
 */
    void utils_write_be16(uint8_t *data, uint16_t value);

    /**
 * @brief 以大端字节序写入 32 位无符号整数
 *
 * @param[out] data 至少包含 4 字节的目标指针
 * @param[in] value 待写入数值
 */
    void utils_write_be32(uint8_t *data, uint32_t value);

    /**
 * @brief 以大端字节序写入 64 位无符号整数
 *
 * @param[out] data 至少包含 8 字节的目标指针
 * @param[in] value 待写入数值
 */
    void utils_write_be64(uint8_t *data, uint64_t value);

    /**
     * @brief 将十六进制字符转换为数值
 *
 * @param[in] character 十六进制字符
 * @return 0 到 15 表示成功，-1 表示字符无效
 */
    int utils_hex_digit_value(char character);

    /**
 * @brief 判断字符串是否为指定长度的十六进制文本
 *
 * @param[in] text 待检查字符串
 * @param[in] length 期望字符数
 * @return true 字符串长度匹配且全部为十六进制字符，false 不匹配
 */
    bool utils_is_hex_string(const char *text, size_t length);

    /**
 * @brief 将字节数组编码为以空字符结尾的十六进制字符串
 *
 * @param[out] destination 目标字符串缓冲区
 * @param[in] destination_size 目标缓冲区容量，至少为 length * 2 + 1
 * @param[in] data 源字节数组
 * @param[in] length 源字节数
 * @param[in] uppercase true 使用大写字母，false 使用小写字母
 * @return true 编码成功，false 参数或缓冲区容量无效
 */
    bool utils_bytes_to_hex(char *destination, size_t destination_size, const uint8_t *data,
                            size_t length, bool uppercase);

    /**
 * @brief 将十六进制字符串解码为字节数组
 *
 * @param[out] destination 目标字节缓冲区
 * @param[in] destination_size 目标缓冲区容量
 * @param[in] text 源十六进制字符串
 * @param[in] text_length 源字符数，必须为偶数
 * @return true 解码成功，false 参数、容量或字符无效
 */
    bool utils_hex_to_bytes(uint8_t *destination, size_t destination_size, const char *text,
                            size_t text_length);

    /**
 * @brief 使用 MSB-first 算法计算 CRC-8
 *
 * @param[in] data 待校验数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @param[in] initial_value CRC 初始值
 * @param[in] polynomial CRC 多项式
 * @return CRC-8 计算结果
 */
    uint8_t utils_crc8_msb(const uint8_t *data, size_t length, uint8_t initial_value,
                           uint8_t polynomial);

    /**
 * @brief 流式更新 CRC-16/CCITT-FALSE 中间值
 *
 * @param[in] crc 当前 CRC 中间值
 * @param[in] data 待校验数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @return 更新后的 CRC 中间值
 */
    uint16_t utils_crc16_ccitt_false_update(uint16_t crc, const uint8_t *data, size_t length);

    /**
 * @brief 计算 CRC-16/CCITT-FALSE
 *
 * @param[in] data 待校验数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @return CRC-16/CCITT-FALSE 结果
 */
    uint16_t utils_crc16_ccitt_false(const uint8_t *data, size_t length);

    /**
 * @brief 流式更新 CRC-16/MODBUS 中间值
 *
 * @param[in] crc 当前 CRC 中间值
 * @param[in] data 待校验数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @return 更新后的 CRC 中间值
 */
    uint16_t utils_crc16_modbus_update(uint16_t crc, const uint8_t *data, size_t length);

    /**
 * @brief 计算 CRC-16/MODBUS
 *
 * @param[in] data 待校验数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @return CRC-16/MODBUS 结果
 */
    uint16_t utils_crc16_modbus(const uint8_t *data, size_t length);

    /**
 * @brief 流式更新 IEEE CRC-32 中间值
 *
 * 首次调用传入 UTILS_CRC32_IEEE_INITIAL_VALUE，处理完所有数据后将返回值与
 * UTILS_CRC32_IEEE_FINAL_XOR 异或即可得到最终 CRC-32。
 *
 * @param[in] crc 当前未最终异或的 CRC-32 中间值
 * @param[in] data 待校验数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @return 更新后的 CRC-32 中间值
 */
    uint32_t utils_crc32_ieee_update(uint32_t crc, const uint8_t *data, size_t length);

    /**
 * @brief 计算完整数据的 IEEE CRC-32
 *
 * @param[in] data 待校验数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @return 最终 IEEE CRC-32 结果
 */
    uint32_t utils_crc32_ieee(const uint8_t *data, size_t length);

    /**
 * @brief 创建 SHA-256 流式计算上下文
 *
 * 调用者完成全部 update 和 final 操作后，必须调用 utils_sha256_destroy() 释放上下文。
 *
 * @param[out] out_context 新建上下文输出指针
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NO_MEM 内存不足；或加密库错误码
 */
    esp_err_t utils_sha256_create(utils_sha256_context_t **out_context);

    /**
 * @brief 向 SHA-256 上下文写入一段数据
 *
 * @param[in,out] context 已创建且尚未完成的 SHA-256 上下文
 * @param[in] data 待写入数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 上下文已完成；或加密库错误码
 */
    esp_err_t utils_sha256_update(utils_sha256_context_t *context, const uint8_t *data,
                                  size_t length);

    /**
 * @brief 完成 SHA-256 流式计算并写出摘要
 *
 * 成功或失败后，上下文均不可再继续 update；调用者仍须调用 utils_sha256_destroy() 释放它。
 *
 * @param[in,out] context 已创建且尚未完成的 SHA-256 上下文
 * @param[out] digest 长度至少为 UTILS_SHA256_DIGEST_SIZE 的摘要缓冲区
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 上下文已完成；或加密库错误码
 */
    esp_err_t utils_sha256_final(utils_sha256_context_t *context,
                                 uint8_t                 digest[UTILS_SHA256_DIGEST_SIZE]);

    /**
 * @brief 释放 SHA-256 流式计算上下文
 *
 * @param[in] context 待释放上下文；可为空指针
 */
    void utils_sha256_destroy(utils_sha256_context_t *context);

    /**
 * @brief 一次性计算数据的 SHA-256 摘要
 *
 * @param[in] data 待计算数据；length 大于零时不可为空指针
 * @param[in] length 数据长度
 * @param[out] digest 长度至少为 UTILS_SHA256_DIGEST_SIZE 的摘要缓冲区
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_NO_MEM 内存不足；或加密库错误码
 */
    esp_err_t utils_sha256(const uint8_t *data, size_t length,
                           uint8_t digest[UTILS_SHA256_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* UTILS_H */
