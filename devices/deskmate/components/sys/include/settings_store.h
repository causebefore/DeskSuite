/**
 * @file settings_store.h
 * @brief DeskMate 产品设置兼容接口
 *
 * PhotoPainter 的通用 system_storage 只覆盖网络、时区和 RTC/OTA 状态。本接口保留
 * DeskMate 独有的 OTA 自动检查、自动安装和通道设置，避免这些产品策略进入通用存储结构。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SETTINGS_WIFI_SSID_MAX     33
#define SETTINGS_WIFI_PASSWORD_MAX 65
#define SETTINGS_SERVICE_URL_MAX   128
#define SETTINGS_DEVICE_TOKEN_MAX  96
#define SETTINGS_OTA_CHANNEL_MAX   16

    typedef struct
    {
        char     wifi_ssid[SETTINGS_WIFI_SSID_MAX];
        char     wifi_password[SETTINGS_WIFI_PASSWORD_MAX];
        char     service_url[SETTINGS_SERVICE_URL_MAX];
        char     device_token[SETTINGS_DEVICE_TOKEN_MAX];
        bool     ota_enabled;
        bool     ota_auto_install;
        uint32_t ota_check_interval_sec;
        char     ota_channel[SETTINGS_OTA_CHANNEL_MAX];
    } device_settings_t;

    /** @brief 用当前产品默认值覆盖完整设置结构 */
    void settings_store_set_defaults(device_settings_t *config);

    /** @brief 有界复制字符串并保证目标以 NUL 结尾 */
    void settings_store_copy_string(char *dst, size_t dst_len, const char *src);

    /** @brief 判断设置是否包含可尝试连接的 Wi-Fi SSID */
    bool settings_store_is_network_ready(const device_settings_t *config);

    /** @brief 初始化设置兼容存储和通用 system_storage */
    esp_err_t settings_store_init(void);

    /**
     * @brief 复制读取完整 DeskMate 产品设置
     *
     * 网络字段只从 system_storage 的通用网络配置读取，不兼容旧
     * deskmate_settings 命名空间中的网络字段。
     */
    esp_err_t settings_store_load_copy(device_settings_t *out_settings);

    /**
     * @brief 同步持久化产品设置与唯一的通用网络配置
     *
     * OTA 产品字段写入 deskmate_settings，网络字段只写入 system_storage。
     */
    esp_err_t settings_store_save(const device_settings_t *config);

    /** @brief 清除 DeskMate 设置和通用网络配置 */
    esp_err_t settings_store_reset(void);

#ifdef __cplusplus
}
#endif
