/**
 * @file system_storage.c
 * @brief 系统持久化存储实现
 */

#include "system_storage.h"

#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

/** @brief 日志标签 */
static const char *TAG = "system_storage";

/** @brief NVS 命名空间 */
#define SYSTEM_STORAGE_NAMESPACE               "system"
/** @brief 时区偏移键 */
#define SYSTEM_STORAGE_KEY_UTC_OFFSET          "utc_offset"
/** @brief 每日告警键 */
#define SYSTEM_STORAGE_KEY_DAILY_ALARM         "daily_alarm"
/** @brief 校时状态键 */
#define SYSTEM_STORAGE_KEY_TIME_SYNCED         "time_synced"
/** @brief 网络配置键 */
#define SYSTEM_STORAGE_KEY_NETWORK_CONFIG      "network_cfg"
/** @brief OTA 提示画面待恢复键 */
#define SYSTEM_STORAGE_KEY_OTA_DISPLAY_RESTORE "ota_disp_rstr"

/**
 * @brief 判断字符串是否在缓冲区内结束
 *
 * @param[in] value 字符串首地址
 * @param[in] capacity 缓冲区容量
 * @return true 已结束，false 未结束
 */
static bool system_storage_string_is_terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

/**
 * @brief 判断网络配置是否合法
 *
 * @param[in] config 网络配置
 * @return true 合法，false 不合法
 */
static bool system_storage_network_config_is_valid(const system_storage_network_config_t *config)
{
    return config != NULL && config->version == SYSTEM_STORAGE_NETWORK_CONFIG_VERSION && config->ssid[0] != '\0'
           && system_storage_string_is_terminated(config->ssid, sizeof(config->ssid))
           && system_storage_string_is_terminated(config->password, sizeof(config->password))
           && system_storage_string_is_terminated(config->service_url, sizeof(config->service_url))
           && system_storage_string_is_terminated(config->device_token, sizeof(config->device_token));
}

/**
 * @brief 判断每日告警配置是否有效
 *
 * @param[in] alarm 每日告警配置
 *
 * @return true 配置有效，false 配置无效
 */
static bool system_storage_daily_alarm_is_valid(const system_storage_daily_alarm_t *alarm)
{
    return alarm != NULL && alarm->hour <= 23U && alarm->minute <= 59U && alarm->second <= 59U;
}

/**
 * @brief 将可预期的 NVS 数据错误归一化为 System 层通用错误
 *
 * @param[in] error NVS 操作结果
 * @return 归一化后的通用错误，或未转换的底层故障码
 */
static esp_err_t system_storage_normalize_nvs_error(esp_err_t error)
{
    switch (error)
    {
        case ESP_ERR_NVS_NOT_FOUND:
            return ESP_ERR_NOT_FOUND;
        case ESP_ERR_NVS_INVALID_LENGTH:
            return ESP_ERR_INVALID_SIZE;
        case ESP_ERR_NVS_TYPE_MISMATCH:
            return ESP_ERR_INVALID_RESPONSE;
        default:
            return error;
    }
}

/**
 * @brief 打开系统 NVS 命名空间
 *
 * @param[in] mode NVS 打开模式
 * @param[out] handle NVS 句柄输出指针
 *
 * @return ESP_OK 成功，或其他错误码
 */
static esp_err_t system_storage_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");
    return system_storage_normalize_nvs_error(nvs_open(SYSTEM_STORAGE_NAMESPACE, mode, handle));
}

esp_err_t system_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS 分区需要重建，将清除已有持久化数据");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "擦除 NVS 分区失败");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "初始化 NVS 失败");
    return ESP_OK;
}

/**
 * @brief 借用并保存完整网络配置
 *
 * @param[in] config 调用期间借用的网络配置
 * @return ESP_OK 成功，或其他错误码
 */
esp_err_t system_storage_set_network_config_borrow(const system_storage_network_config_t *config)
{
    ESP_RETURN_ON_FALSE(system_storage_network_config_is_valid(config), ESP_ERR_INVALID_ARG, TAG, "网络配置无效");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READWRITE, &handle), TAG, "打开 NVS 失败");
    esp_err_t err = nvs_set_blob(handle, SYSTEM_STORAGE_KEY_NETWORK_CONFIG, config, sizeof(*config));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return system_storage_normalize_nvs_error(err);
}

/**
 * @brief 复制读取完整网络配置
 *
 * @param[out] out_config 网络配置输出指针，仅在返回 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示不存在；
 *         ESP_ERR_INVALID_SIZE 或 ESP_ERR_INVALID_RESPONSE 表示数据不兼容；或其他错误码
 */
esp_err_t system_storage_get_network_config_copy(system_storage_network_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READONLY, &handle), TAG, "打开 NVS 失败");
    system_storage_network_config_t config = { 0 };
    size_t                          size   = sizeof(config);
    esp_err_t                       err    = nvs_get_blob(handle, SYSTEM_STORAGE_KEY_NETWORK_CONFIG, &config, &size);
    nvs_close(handle);
    if (err != ESP_OK)
    {
        return system_storage_normalize_nvs_error(err);
    }
    ESP_RETURN_ON_FALSE(size == sizeof(config), ESP_ERR_INVALID_SIZE, TAG, "网络配置长度无效");
    ESP_RETURN_ON_FALSE(system_storage_network_config_is_valid(&config),
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "网络配置内容无效");
    *out_config = config;
    return ESP_OK;
}

/**
 * @brief 清除完整网络配置
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示不存在；或其他错误码
 */
esp_err_t system_storage_erase_network_config(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READWRITE, &handle), TAG, "打开 NVS 失败");
    esp_err_t err = nvs_erase_key(handle, SYSTEM_STORAGE_KEY_NETWORK_CONFIG);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return system_storage_normalize_nvs_error(err);
}

esp_err_t system_storage_set_utc_offset_minutes(int16_t utc_offset_minutes)
{
    ESP_RETURN_ON_FALSE(utc_offset_minutes >= -720 && utc_offset_minutes <= 840,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "时区偏移超出范围");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READWRITE, &handle), TAG, "打开 NVS 失败");
    esp_err_t err = nvs_set_i16(handle, SYSTEM_STORAGE_KEY_UTC_OFFSET, utc_offset_minutes);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return system_storage_normalize_nvs_error(err);
}

esp_err_t system_storage_get_utc_offset_minutes(int16_t *utc_offset_minutes)
{
    ESP_RETURN_ON_FALSE(utc_offset_minutes != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READONLY, &handle), TAG, "打开 NVS 失败");
    const esp_err_t err = nvs_get_i16(handle, SYSTEM_STORAGE_KEY_UTC_OFFSET, utc_offset_minutes);
    nvs_close(handle);
    return system_storage_normalize_nvs_error(err);
}

/**
 * @brief 借用并保存每日 RTC 告警配置
 *
 * @param[in] alarm 调用期间借用的每日告警时间
 * @return ESP_OK 成功；或参数、NVS 错误码
 */
esp_err_t system_storage_set_daily_alarm_borrow(const system_storage_daily_alarm_t *alarm)
{
    ESP_RETURN_ON_FALSE(system_storage_daily_alarm_is_valid(alarm), ESP_ERR_INVALID_ARG, TAG, "每日告警配置无效");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READWRITE, &handle), TAG, "打开 NVS 失败");
    esp_err_t err = nvs_set_blob(handle, SYSTEM_STORAGE_KEY_DAILY_ALARM, alarm, sizeof(*alarm));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return system_storage_normalize_nvs_error(err);
}

/**
 * @brief 复制读取每日 RTC 告警配置
 *
 * @param[out] out_alarm 每日告警时间，仅在返回 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 尚未保存；
 *         ESP_ERR_INVALID_SIZE 或 ESP_ERR_INVALID_RESPONSE 表示数据不兼容；或其他错误码
 */
esp_err_t system_storage_get_daily_alarm_copy(system_storage_daily_alarm_t *out_alarm)
{
    ESP_RETURN_ON_FALSE(out_alarm != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READONLY, &handle), TAG, "打开 NVS 失败");

    system_storage_daily_alarm_t alarm = { 0 };
    size_t                       size  = sizeof(alarm);
    esp_err_t                    err   = nvs_get_blob(handle, SYSTEM_STORAGE_KEY_DAILY_ALARM, &alarm, &size);
    nvs_close(handle);
    if (err != ESP_OK)
    {
        return system_storage_normalize_nvs_error(err);
    }
    ESP_RETURN_ON_FALSE(size == sizeof(alarm), ESP_ERR_INVALID_SIZE, TAG, "每日告警数据长度无效");
    ESP_RETURN_ON_FALSE(system_storage_daily_alarm_is_valid(&alarm), ESP_ERR_INVALID_RESPONSE, TAG, "每日告警数据无效");
    *out_alarm = alarm;
    return ESP_OK;
}

esp_err_t system_storage_set_time_synced(bool synced)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READWRITE, &handle), TAG, "打开 NVS 失败");
    esp_err_t err = nvs_set_u8(handle, SYSTEM_STORAGE_KEY_TIME_SYNCED, synced ? 1U : 0U);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return system_storage_normalize_nvs_error(err);
}

esp_err_t system_storage_get_time_synced(bool *synced)
{
    ESP_RETURN_ON_FALSE(synced != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READONLY, &handle), TAG, "打开 NVS 失败");

    uint8_t   value;
    esp_err_t err = nvs_get_u8(handle, SYSTEM_STORAGE_KEY_TIME_SYNCED, &value);
    nvs_close(handle);
    err = system_storage_normalize_nvs_error(err);
    ESP_RETURN_ON_ERROR(err, TAG, "读取校时状态失败");
    ESP_RETURN_ON_FALSE(value <= 1U, ESP_ERR_INVALID_RESPONSE, TAG, "校时状态数据无效");

    *synced = value != 0U;
    return ESP_OK;
}

esp_err_t system_storage_set_ota_display_restore_pending(bool pending)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READWRITE, &handle), TAG, "打开 NVS 失败");
    esp_err_t err = nvs_set_u8(handle, SYSTEM_STORAGE_KEY_OTA_DISPLAY_RESTORE, pending ? 1U : 0U);
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return system_storage_normalize_nvs_error(err);
}

esp_err_t system_storage_get_ota_display_restore_pending(bool *out_pending)
{
    ESP_RETURN_ON_FALSE(out_pending != NULL, ESP_ERR_INVALID_ARG, TAG, "OTA 画面恢复状态输出指针为空");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(system_storage_open(NVS_READONLY, &handle), TAG, "打开 NVS 失败");
    uint8_t   value = 0U;
    esp_err_t err   = nvs_get_u8(handle, SYSTEM_STORAGE_KEY_OTA_DISPLAY_RESTORE, &value);
    nvs_close(handle);
    err = system_storage_normalize_nvs_error(err);
    if (err != ESP_OK)
    {
        return err;
    }
    ESP_RETURN_ON_FALSE(value <= 1U, ESP_ERR_INVALID_RESPONSE, TAG, "OTA 画面恢复状态持久化值非法");
    *out_pending = value != 0U;
    return ESP_OK;
}
