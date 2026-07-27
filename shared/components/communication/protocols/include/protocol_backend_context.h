/**
 * @file protocol_backend_context.h
 * @brief 定义所有设备后端协议共用的连接与身份上下文
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol_identity.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 后端基础 URL 缓冲区容量（含结尾空字符） */
#define PROTOCOL_BACKEND_BASE_URL_MAX        192U
/** @brief 设备共享令牌缓冲区容量（含结尾空字符） */
#define PROTOCOL_BACKEND_TOKEN_MAX           96U
/** @brief 固件兼容目标缓冲区容量（含结尾空字符） */
#define PROTOCOL_BACKEND_FIRMWARE_TARGET_MAX 65U

/**
 * @brief 后端上下文构造输入
 *
 * `device_id` 为空时使用 Wi-Fi Station 基础 MAC 生成稳定硬件设备
 * ID。所有输入只在 `protocol_backend_context_build_copy()` 调用期间借用。
 */
typedef struct
{
    const char *base_url;        /**< 非空后端基础 URL */
    const char *token;           /**< 可为空的设备共享令牌 */
    const char *device_id;       /**< 可为空；为空时生成稳定硬件设备 ID */
    uint32_t    product_id;      /**< 大于零的产品标识 */
    const char *firmware_target; /**< 固件兼容目标 */
} protocol_backend_context_config_t;

/**
 * @brief 后端连接、鉴权与设备身份的完整值快照
 *
 * 该结构不拥有 Task、锁或持久化状态，可按值复制。通信协议只能读取这些事实，
 * 不得在其中保存重试、安装、校时可信度或其他产品策略。
 */
typedef struct
{
    char     base_url[PROTOCOL_BACKEND_BASE_URL_MAX];               /**< 后端基础 URL */
    char     token[PROTOCOL_BACKEND_TOKEN_MAX];                     /**< 设备共享令牌 */
    char     device_id[PROTOCOL_IDENTITY_DEVICE_ID_MAX];            /**< 稳定设备 ID */
    uint32_t product_id;                                            /**< 产品标识 */
    char     firmware_target[PROTOCOL_BACKEND_FIRMWARE_TARGET_MAX]; /**< 固件兼容目标 */
} protocol_backend_context_t;

/**
 * @brief 校验输入并构造完整后端上下文副本
 *
 * `firmware_target`
 * 必须以小写字母开头，且只包含小写字母、数字和下划线。`device_id` 为空时从
 * Wi-Fi Station 基础 MAC 生成 `esp32-xxxxxxxxxxxx`。
 *
 * @param[in] config 构造输入，仅在调用期间借用
 * @param[out] out_context 完整上下文副本，仅在 ESP_OK 时有效
 * @return ESP_OK 构造成功；ESP_ERR_INVALID_ARG 字段无效或过长；
 *         或读取硬件 MAC 的错误码
 */
esp_err_t protocol_backend_context_build_copy(const protocol_backend_context_config_t *config,
                                              protocol_backend_context_t              *out_context);

/**
 * @brief 校验已有后端上下文是否完整且可安全使用
 *
 * @param[in] context 待校验上下文
 * @return true 全部字段合法；false 上下文为空、未结束或字段不合法
 */
bool protocol_backend_context_is_valid(const protocol_backend_context_t *context);

#ifdef __cplusplus
}
#endif
