/**
 * @file libcrc.c
 * @brief 实现项目常用的 CRC 校验算法
 */
#include "utils.h"

/** @brief IEEE CRC-32 的反射多项式 */
#define UTILS_CRC32_IEEE_POLYNOMIAL        0xEDB88320U
/** @brief CRC-16/CCITT-FALSE 多项式 */
#define UTILS_CRC16_CCITT_FALSE_POLYNOMIAL 0x1021U
/** @brief CRC-16/MODBUS 的反射多项式 */
#define UTILS_CRC16_MODBUS_POLYNOMIAL      0xA001U

/**
 * @brief 使用 MSB-first 算法计算 CRC-8
 *
 * @param[in] data 待校验数据
 * @param[in] length 数据长度
 * @param[in] initial_value CRC 初始值
 * @param[in] polynomial CRC 多项式
 * @return CRC-8 计算结果
 */
uint8_t utils_crc8_msb(const uint8_t *data, size_t length, uint8_t initial_value,
                       uint8_t polynomial)
{
    uint8_t crc = initial_value;
    for (size_t index = 0U; index < length; ++index)
    {
        crc ^= data[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit)
        {
            crc =
                (crc & 0x80U) != 0U ? (uint8_t) ((crc << 1U) ^ polynomial) : (uint8_t) (crc << 1U);
        }
    }
    return crc;
}

/**
 * @brief 流式更新 CRC-16/CCITT-FALSE 中间值
 *
 * @param[in] crc 当前 CRC 中间值
 * @param[in] data 待校验数据
 * @param[in] length 数据长度
 * @return 更新后的 CRC 中间值
 */
uint16_t utils_crc16_ccitt_false_update(uint16_t crc, const uint8_t *data, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        crc ^= (uint16_t) data[index] << 8U;
        for (uint8_t bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc & 0x8000U) != 0U
                      ? (uint16_t) ((crc << 1U) ^ UTILS_CRC16_CCITT_FALSE_POLYNOMIAL)
                      : (uint16_t) (crc << 1U);
        }
    }
    return crc;
}

/**
 * @brief 计算 CRC-16/CCITT-FALSE
 *
 * @param[in] data 待校验数据
 * @param[in] length 数据长度
 * @return CRC-16/CCITT-FALSE 结果
 */
uint16_t utils_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    return utils_crc16_ccitt_false_update(UTILS_CRC16_CCITT_FALSE_INITIAL_VALUE, data, length);
}

/**
 * @brief 流式更新 CRC-16/MODBUS 中间值
 *
 * @param[in] crc 当前 CRC 中间值
 * @param[in] data 待校验数据
 * @param[in] length 数据长度
 * @return 更新后的 CRC 中间值
 */
uint16_t utils_crc16_modbus_update(uint16_t crc, const uint8_t *data, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        crc ^= data[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc & 1U) != 0U ? (uint16_t) ((crc >> 1U) ^ UTILS_CRC16_MODBUS_POLYNOMIAL)
                                   : (uint16_t) (crc >> 1U);
        }
    }
    return crc;
}

/**
 * @brief 计算 CRC-16/MODBUS
 *
 * @param[in] data 待校验数据
 * @param[in] length 数据长度
 * @return CRC-16/MODBUS 结果
 */
uint16_t utils_crc16_modbus(const uint8_t *data, size_t length)
{
    return utils_crc16_modbus_update(UTILS_CRC16_MODBUS_INITIAL_VALUE, data, length);
}

/**
 * @brief 流式更新 IEEE CRC-32 中间值
 *
 * @param[in] crc 当前未最终异或的 CRC-32 中间值
 * @param[in] data 待校验数据
 * @param[in] length 数据长度
 * @return 更新后的 CRC-32 中间值
 */
uint32_t utils_crc32_ieee_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        crc ^= data[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ UTILS_CRC32_IEEE_POLYNOMIAL : crc >> 1U;
        }
    }
    return crc;
}

/**
 * @brief 计算完整数据的 IEEE CRC-32
 *
 * @param[in] data 待校验数据
 * @param[in] length 数据长度
 * @return 最终 IEEE CRC-32 结果
 */
uint32_t utils_crc32_ieee(const uint8_t *data, size_t length)
{
    return utils_crc32_ieee_update(UTILS_CRC32_IEEE_INITIAL_VALUE, data, length)
           ^ UTILS_CRC32_IEEE_FINAL_XOR;
}
