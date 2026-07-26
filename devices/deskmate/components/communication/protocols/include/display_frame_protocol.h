/**
 * @file display_frame_protocol.h
 * @brief PPF2/GRAY2 显示帧校验与只读像素视图
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "display_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief PPF2 固定头部字节数 */
#define DISPLAY_FRAME_PROTOCOL_HEADER_SIZE    32U
/** @brief PPF2 格式名称 */
#define DISPLAY_FRAME_PROTOCOL_FORMAT         "PPF2"
/** @brief PPF2 固定格式版本 */
#define DISPLAY_FRAME_PROTOCOL_VERSION        2U
/** @brief GRAY2 像素格式名称 */
#define DISPLAY_FRAME_PROTOCOL_PIXEL_FORMAT   "GRAY2"
/** @brief GRAY2 固定位深 */
#define DISPLAY_FRAME_PROTOCOL_BITS_PER_PIXEL 2U
/** @brief GRAY2 固定像素负载字节数 */
#define DISPLAY_FRAME_PROTOCOL_PAYLOAD_SIZE   96000U
/** @brief PPF2 完整文件固定字节数 */
#define DISPLAY_FRAME_PROTOCOL_FILE_SIZE      96032U
/** @brief GRAY2 固定画面宽度 */
#define DISPLAY_FRAME_PROTOCOL_WIDTH          800U
/** @brief GRAY2 固定画面高度 */
#define DISPLAY_FRAME_PROTOCOL_HEIGHT         480U
/** @brief GRAY2 每行固定字节数 */
#define DISPLAY_FRAME_PROTOCOL_STRIDE         200U

    /** @brief 校验通过后借用的 GRAY2 像素视图 */
    typedef struct
    {
        const uint8_t *pixels;       /**< 借用的像素负载首地址 */
        size_t         size_bytes;   /**< 像素负载字节数 */
        size_t         stride_bytes; /**< 每行字节数 */
        uint16_t       width;        /**< 像素宽度 */
        uint16_t       height;       /**< 像素高度 */
    } display_frame_protocol_view_t;

    /**
 * @brief 校验完整 PPF2 文件并借用其中的 GRAY2 像素负载
 *
 * 校验固定头字段、content_version、payload CRC32、完整文件 SHA-256 和 payload
 * SHA-256。函数返回后不保存任何输入指针，输出视图的有效期不超过 @p frame_data。
 *
 * @param[in] frame_data 完整 PPF2 文件数据
 * @param[in] frame_size 文件字节数
 * @param[in] expected_page Manifest 中对应页面元数据
 * @param[out] out_view 校验通过后的借用像素视图
 * @return ESP_OK 校验通过；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_SIZE 长度错误；
 *         ESP_ERR_INVALID_RESPONSE 帧头或摘要不匹配；或 SHA-256 计算错误码
 */
    esp_err_t display_frame_protocol_validate_borrow(const uint8_t *frame_data, size_t frame_size,
                                                     const display_protocol_page_t *expected_page,
                                                     display_frame_protocol_view_t *out_view);

#ifdef __cplusplus
}
#endif
