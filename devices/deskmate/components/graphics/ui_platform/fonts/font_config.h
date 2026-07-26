/*
 * 文件职责：定义字体服务运行时使用的公共常量，供容器层、适配层和服务编排层共享。
 * 主要依赖：无。
 * 调用方：ui_platform 字体容器层和适配层。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief RLCD 字体文件魔数 */
#define RLCD_FONT_MAGIC            "RLCD"
/** @brief RLCD 字体格式版本 */
#define RLCD_FONT_VERSION          1U
/** @brief RLCD 字体头大小（字节） */
#define RLCD_FONT_HEADER_SIZE      8U
/** @brief RLCD 字体索引条目大小（字节）: id:u8 + size_px:u16 + offset:u32 + length:u32 */
#define RLCD_FONT_INDEX_ENTRY_SIZE 11U
/** @brief 最大字体块数量 */
#define RLCD_FONT_MAX_BLOCKS       32U

/** @brief 必需字体块掩码（当前要求常规、半粗和数字字体块都存在） */
#define RLCD_FONT_REQUIRED_MASK    0x3fU

    /**
 * @brief RLCD 字体 ID 枚举
 */
    typedef enum
    {
        RLCD_FONT_ID_ZH_16          = 0, /*!< 16px 中文字体 */
        RLCD_FONT_ID_ZH_24          = 1, /*!< 24px 中文字体 */
        RLCD_FONT_ID_ZH_32          = 2, /*!< 32px 中文字体 */
        RLCD_FONT_ID_NUM_48         = 3, /*!< 48px 数字字体 */
        RLCD_FONT_ID_ZH_16_SEMIBOLD = 4, /*!< 16px 半粗中文字体 */
        RLCD_FONT_ID_ZH_24_SEMIBOLD = 5, /*!< 24px 半粗中文字体 */
    } rlcd_font_id_t;

#ifdef __cplusplus
}
#endif
