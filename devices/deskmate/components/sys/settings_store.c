/**
 * @file settings_store.c
 * @brief 保留 DeskMate 原有 NVS schema 的产品设置兼容实现
 */
#include "settings_store.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "system_storage.h"

static const char *TAG = "settings_store";

#ifndef CONFIG_DESKMATE_SERVER_URL
    #define CONFIG_DESKMATE_SERVER_URL ""
#endif

#ifndef CONFIG_DESKMATE_OTA_CHECK_INTERVAL_SEC
    #define CONFIG_DESKMATE_OTA_CHECK_INTERVAL_SEC 1800
#endif

#define SETTINGS_STORE_LEGACY_OTA_INTERVAL_SEC 15U

static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static bool              s_initialized;

static esp_err_t ensure_settings_lock(void)
{
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    }
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t open_config_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(CONFIG_DESKMATE_SETTINGS_NAMESPACE, mode, handle);
}

static void copy_network_from_system_storage(device_settings_t                     *settings,
                                             const system_storage_network_config_t *network)
{
    settings_store_copy_string(settings->wifi_ssid, sizeof(settings->wifi_ssid), network->ssid);
    settings_store_copy_string(settings->wifi_password, sizeof(settings->wifi_password), network->password);
    settings_store_copy_string(settings->service_url, sizeof(settings->service_url), network->service_url);
    settings_store_copy_string(settings->device_token, sizeof(settings->device_token), network->device_token);
}

static esp_err_t save_network_to_system_storage(const device_settings_t *settings)
{
    if (settings->wifi_ssid[0] == '\0')
    {
        const esp_err_t erase_error = system_storage_erase_network_config();
        return erase_error == ESP_ERR_NOT_FOUND ? ESP_OK : erase_error;
    }
    system_storage_network_config_t network = {
        .version = SYSTEM_STORAGE_NETWORK_CONFIG_VERSION,
    };
    settings_store_copy_string(network.ssid, sizeof(network.ssid), settings->wifi_ssid);
    settings_store_copy_string(network.password, sizeof(network.password), settings->wifi_password);
    settings_store_copy_string(network.service_url, sizeof(network.service_url), settings->service_url);
    settings_store_copy_string(network.device_token, sizeof(network.device_token), settings->device_token);
    return system_storage_set_network_config_borrow(&network);
}

void settings_store_copy_string(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0)
    {
        return;
    }
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    size_t copy_len = strlen(src);
    if (copy_len >= dst_len)
    {
        copy_len = dst_len - 1;
    }
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

void settings_store_set_defaults(device_settings_t *config)
{
    if (config == NULL)
    {
        return;
    }
    memset(config, 0, sizeof(*config));
#ifdef CONFIG_DESKMATE_OTA_DEFAULT_ENABLED
    config->ota_enabled = true;
#endif
#ifdef CONFIG_DESKMATE_OTA_AUTO_INSTALL
    config->ota_auto_install = true;
#endif
    config->ota_check_interval_sec = CONFIG_DESKMATE_OTA_CHECK_INTERVAL_SEC;
    settings_store_copy_string(config->service_url, sizeof(config->service_url), CONFIG_DESKMATE_SERVER_URL);
    settings_store_copy_string(config->ota_channel, sizeof(config->ota_channel), "test");
}

bool settings_store_is_network_ready(const device_settings_t *config)
{
    return config != NULL && config->wifi_ssid[0] != '\0';
}

esp_err_t settings_store_init(void)
{
    ESP_RETURN_ON_ERROR(ensure_settings_lock(), TAG, "创建设置存储互斥锁失败");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_initialized)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    const esp_err_t error = system_storage_init();
    if (error == ESP_OK)
    {
        s_initialized = true;
    }
    xSemaphoreGive(s_lock);
    return error;
}

esp_err_t settings_store_load_copy(device_settings_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "设置输出为空");
    ESP_RETURN_ON_ERROR(settings_store_init(), TAG, "初始化设置存储失败");
    settings_store_set_defaults(out);
    system_storage_network_config_t network          = { 0 };
    const esp_err_t                 network_error    = system_storage_get_network_config_copy(&network);
    const bool                      have_new_network = network_error == ESP_OK;
    if (network_error != ESP_OK && network_error != ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "读取通用网络配置失败，继续尝试旧 schema: %s", esp_err_to_name(network_error));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t nvs   = 0;
    esp_err_t    error = open_config_nvs(NVS_READONLY, &nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND)
    {
        xSemaphoreGive(s_lock);
        if (have_new_network)
        {
            copy_network_from_system_storage(out, &network);
        }
        return ESP_OK;
    }
    if (error != ESP_OK)
    {
        xSemaphoreGive(s_lock);
        return error;
    }

    size_t length = sizeof(out->wifi_ssid);
    (void) nvs_get_str(nvs, "ssid", out->wifi_ssid, &length);
    length = sizeof(out->wifi_password);
    (void) nvs_get_str(nvs, "pass", out->wifi_password, &length);
    length = sizeof(out->service_url);
    (void) nvs_get_str(nvs, "service", out->service_url, &length);
    length = sizeof(out->device_token);
    (void) nvs_get_str(nvs, "token", out->device_token, &length);

    uint8_t flag = 0;
    if (nvs_get_u8(nvs, "ota_en", &flag) == ESP_OK)
    {
        out->ota_enabled = flag != 0;
    }
    if (nvs_get_u8(nvs, "ota_auto", &flag) == ESP_OK)
    {
        out->ota_auto_install = flag != 0;
    }
    if (nvs_get_u32(nvs, "ota_int", &out->ota_check_interval_sec) == ESP_OK
        && out->ota_check_interval_sec == SETTINGS_STORE_LEGACY_OTA_INTERVAL_SEC)
    {
        out->ota_check_interval_sec = CONFIG_DESKMATE_OTA_CHECK_INTERVAL_SEC;
    }
    length = sizeof(out->ota_channel);
    (void) nvs_get_str(nvs, "ota_ch", out->ota_channel, &length);
    nvs_close(nvs);
    xSemaphoreGive(s_lock);
    if (have_new_network)
    {
        copy_network_from_system_storage(out, &network);
    }
    else if (out->wifi_ssid[0] != '\0')
    {
        const esp_err_t migration_error = save_network_to_system_storage(out);
        if (migration_error != ESP_OK)
        {
            ESP_LOGW(TAG, "迁移旧网络配置到 system_storage 失败: %s", esp_err_to_name(migration_error));
        }
    }
    return ESP_OK;
}

esp_err_t settings_store_save(const device_settings_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "设置输入为空");
    ESP_RETURN_ON_ERROR(settings_store_init(), TAG, "初始化设置存储失败");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t nvs   = 0;
    esp_err_t    error = open_config_nvs(NVS_READWRITE, &nvs);
#define SAVE_IF_OK(expression)    \
    do                            \
    {                             \
        if (error == ESP_OK)      \
        {                         \
            error = (expression); \
        }                         \
    }                             \
    while (0)
    SAVE_IF_OK(nvs_set_str(nvs, "ssid", config->wifi_ssid));
    SAVE_IF_OK(nvs_set_str(nvs, "pass", config->wifi_password));
    SAVE_IF_OK(nvs_set_str(nvs, "service", config->service_url));
    SAVE_IF_OK(nvs_set_str(nvs, "token", config->device_token));
    SAVE_IF_OK(nvs_set_u8(nvs, "ota_en", config->ota_enabled ? 1 : 0));
    SAVE_IF_OK(nvs_set_u8(nvs, "ota_auto", config->ota_auto_install ? 1 : 0));
    SAVE_IF_OK(nvs_set_u32(nvs, "ota_int", config->ota_check_interval_sec));
    SAVE_IF_OK(nvs_set_str(nvs, "ota_ch", config->ota_channel));
    SAVE_IF_OK(nvs_commit(nvs));
#undef SAVE_IF_OK
    if (nvs != 0)
    {
        nvs_close(nvs);
    }
    xSemaphoreGive(s_lock);
    if (error != ESP_OK)
    {
        return error;
    }
    return save_network_to_system_storage(config);
}

esp_err_t settings_store_reset(void)
{
    ESP_RETURN_ON_ERROR(settings_store_init(), TAG, "初始化设置存储失败");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t nvs   = 0;
    esp_err_t    error = open_config_nvs(NVS_READWRITE, &nvs);
    if (error == ESP_OK)
    {
        error = nvs_erase_all(nvs);
    }
    if (error == ESP_OK)
    {
        error = nvs_commit(nvs);
    }
    if (nvs != 0)
    {
        nvs_close(nvs);
    }
    xSemaphoreGive(s_lock);
    const esp_err_t network_error = system_storage_erase_network_config();
    if (error != ESP_OK)
    {
        return error;
    }
    return network_error == ESP_ERR_NOT_FOUND ? ESP_OK : network_error;
}
