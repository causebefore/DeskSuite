/**
 * @file system_storage.h
 * @brief 系统持久化存储接口
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SYSTEM_STORAGE_NETWORK_SSID_MAX        33
#define SYSTEM_STORAGE_NETWORK_PASSWORD_MAX    65
#define SYSTEM_STORAGE_NETWORK_SERVICE_URL_MAX 128
#define SYSTEM_STORAGE_NETWORK_TOKEN_MAX       96
#define SYSTEM_STORAGE_NETWORK_CONFIG_VERSION  1U

    /** @brief 持久化的网络配置 */
    typedef struct
    {
        uint16_t version; /**< 数据版本 */
        char     ssid[SYSTEM_STORAGE_NETWORK_SSID_MAX];
        char     password[SYSTEM_STORAGE_NETWORK_PASSWORD_MAX];
        char     service_url[SYSTEM_STORAGE_NETWORK_SERVICE_URL_MAX];
        char     device_token[SYSTEM_STORAGE_NETWORK_TOKEN_MAX];
    } system_storage_network_config_t;

    /**
 * @brief 初始化系统持久化存储
 *
 * @return ESP_OK 成功，或其他错误码
 */
    esp_err_t system_storage_init(void);

    /**
 * @brief 借用并保存完整网络配置
 *
 * 函数返回前完成持久化，不保留调用方指针。
 *
 * @param[in] config 调用期间借用的网络配置
 * @return ESP_OK 成功，或其他错误码
 */
    esp_err_t system_storage_set_network_config_borrow(const system_storage_network_config_t *config);

    /**
 * @brief 复制读取完整网络配置
 *
 * @param[out] out_config 网络配置输出指针，仅在返回 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示不存在；
 *         ESP_ERR_INVALID_SIZE 或 ESP_ERR_INVALID_RESPONSE 表示数据不兼容；或其他错误码
 */
    esp_err_t system_storage_get_network_config_copy(system_storage_network_config_t *out_config);

    /**
 * @brief 清除完整网络配置
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示不存在；或其他错误码
 */
    esp_err_t system_storage_erase_network_config(void);

    /**
 * @brief 保存本地时区相对 UTC 的偏移分钟数
 *
 * @param[in] utc_offset_minutes UTC 偏移分钟数，范围 -720~840
 *
 * @return ESP_OK 成功，或其他错误码
 */
    esp_err_t system_storage_set_utc_offset_minutes(int16_t utc_offset_minutes);

    /**
 * @brief 读取本地时区相对 UTC 的偏移分钟数
 *
 * @param[out] utc_offset_minutes UTC 偏移分钟数输出指针
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示尚未保存；
 *         ESP_ERR_INVALID_SIZE 或 ESP_ERR_INVALID_RESPONSE 表示数据不兼容；或其他错误码
 */
    esp_err_t system_storage_get_utc_offset_minutes(int16_t *utc_offset_minutes);

#ifdef __cplusplus
}
#endif
