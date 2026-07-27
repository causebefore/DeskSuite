/**
 * @file display_protocol.h
 * @brief 显示 Manifest 与帧下载协议
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol_backend_context.h"
#include "transport.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 日期时间版本字符串容量 */
#define DISPLAY_PROTOCOL_VERSION_MAX 16U
/** @brief SHA-256 十六进制字符串容量 */
#define DISPLAY_PROTOCOL_SHA256_MAX  65U
/** @brief 帧相对 URL 容量 */
#define DISPLAY_PROTOCOL_URL_MAX     192U
/** @brief 页面 ID 字符串容量 */
#define DISPLAY_PROTOCOL_PAGE_ID_MAX 81U
/** @brief 单个集合允许的最大页面数量 */
#define DISPLAY_PROTOCOL_PAGE_MAX    16U

    /** @brief Manifest v3 中的单个四灰阶页面 */
    typedef struct
    {
        char     page_id[DISPLAY_PROTOCOL_PAGE_ID_MAX];         /**< 页面稳定标识 */
        char     content_version[DISPLAY_PROTOCOL_VERSION_MAX]; /**< YYYYMMDD-HHMMSS */
        uint32_t file_size;                                     /**< 完整 PPF2 文件长度 */
        uint32_t crc32;                                         /**< payload CRC32 */
        char     sha256[DISPLAY_PROTOCOL_SHA256_MAX];           /**< 完整文件 SHA-256 */
        char     payload_sha256[DISPLAY_PROTOCOL_SHA256_MAX];   /**< 像素负载 SHA-256 */
        char     frame_url[DISPLAY_PROTOCOL_URL_MAX];           /**< 同源帧相对 URL */
    } display_protocol_page_t;

    /** @brief 多页面显示集合 Manifest v3 */
    typedef struct
    {
        bool                    not_modified; /**< 服务端返回 304 */
        char                    collection_version[DISPLAY_PROTOCOL_VERSION_MAX]; /**< 集合版本 */
        char                    default_page[DISPLAY_PROTOCOL_PAGE_ID_MAX]; /**< 默认显示页面 */
        uint16_t                width;                                      /**< 画面宽度 */
        uint16_t                height;                                     /**< 画面高度 */
        uint8_t                 bits_per_pixel;                             /**< 每像素位数 */
        uint16_t                header_size;                                /**< PPF2 头部长度 */
        uint32_t                payload_size;                               /**< 像素负载长度 */
        uint32_t                file_size;                                  /**< 单页完整文件长度 */
        uint8_t                 page_count;                                 /**< 有效页面数量 */
        display_protocol_page_t pages[DISPLAY_PROTOCOL_PAGE_MAX];           /**< 页面列表 */
        int64_t                 next_refresh_at_utc;                        /**< 下一次刷新 UTC Unix 秒 */
    } display_protocol_manifest_t;

    /**
 * @brief 同步查询并复制设备当前显示 Manifest
 *
 * 本函数仅在同步请求期间借用所有输入字符串，返回后不会保存这些指针。
 * 完整 Manifest 会复制到 @p out_manifest，且仅在返回 ESP_OK 时有效。
 *
 * @param[in] in_backend 后端连接、鉴权与设备身份上下文
 * @param[in] in_current_version 当前集合版本，可为空
 * @param[in] in_current_next_refresh_at_utc 当前集合刷新目标；与版本共同构造 ETag
 * @param[in] timeout_ms 请求超时
 * @param[out] out_manifest Manifest
 * @return ESP_OK 成功，或网络、HTTP、JSON 校验错误码
 */
    esp_err_t display_protocol_get_manifest_copy(const protocol_backend_context_t *in_backend,
                                                 const char *in_current_version,
                                                 int64_t in_current_next_refresh_at_utc,
                                                 int timeout_ms,
                                                 display_protocol_manifest_t *out_manifest);

    /**
 * @brief 同步流式下载 Manifest 指定的 PPF 文件
 *
 * 本函数仅在同步请求期间借用后端上下文、帧地址、回调和回调上下文，
 * 返回后不会保存或转交这些指针。下载回调在本函数返回前执行；下载结果仅在
 * 返回 ESP_OK 时有效。
 *
 * @param[in] in_backend 后端连接、鉴权与设备身份上下文
 * @param[in] in_frame_url Manifest 中的同源相对地址
 * @param[in] timeout_ms 请求超时
 * @param[in] in_callback 响应数据回调
 * @param[in] in_context 用户上下文
 * @param[out] out_result 下载结果
 * @return ESP_OK 成功，或 URL、HTTP、回调错误码
 */
    esp_err_t display_protocol_download_frame_borrow(const protocol_backend_context_t *in_backend,
                                                     const char *in_frame_url, int timeout_ms,
                                                     transport_http_data_cb_t          in_callback,
                                                     void                             *in_context,
                                                     transport_http_download_result_t *out_result);

#ifdef __cplusplus
}
#endif
