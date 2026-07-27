/**
 * @file pomodoro_store.c
 * @brief 使用独立 NVS 命名空间保存番茄钟设置和完成计数
 */
#include "pomodoro_store.h"

#include "nvs.h"

#define POMODORO_NAMESPACE "pomodoro"

static bool settings_are_valid(const pomodoro_store_settings_t *settings)
{
    return settings != NULL && settings->focus_minutes >= 5U && settings->focus_minutes <= 90U
           && (settings->focus_minutes % 5U) == 0U && settings->short_break_minutes >= 1U
           && settings->short_break_minutes <= 30U && settings->long_break_minutes >= 5U
           && settings->long_break_minutes <= 60U && (settings->long_break_minutes % 5U) == 0U
           && settings->long_break_interval >= 2U && settings->long_break_interval <= 8U;
}

static esp_err_t open_store(nvs_open_mode_t mode, nvs_handle_t *out_handle)
{
    if (out_handle == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return nvs_open(POMODORO_NAMESPACE, mode, out_handle);
}

esp_err_t pomodoro_store_init(void)
{
    nvs_handle_t    handle;
    const esp_err_t error = open_store(NVS_READWRITE, &handle);
    if (error == ESP_OK)
    {
        nvs_close(handle);
    }
    return error;
}

esp_err_t pomodoro_store_load_copy(pomodoro_store_snapshot_t *out_snapshot)
{
    if (out_snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    pomodoro_store_snapshot_t snapshot = { 0 };
    nvs_handle_t              handle;
    esp_err_t                 error = open_store(NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND)
    {
        *out_snapshot = snapshot;
        return ESP_OK;
    }
    if (error != ESP_OK)
    {
        return error;
    }

    uint8_t schema = 0U;
    snapshot.schema_valid =
        nvs_get_u8(handle, "schema_ver", &schema) == ESP_OK && schema == POMODORO_STORE_SCHEMA_VERSION;
    const bool has_focus       = nvs_get_u8(handle, "focus_min", &snapshot.settings.focus_minutes) == ESP_OK;
    const bool has_short       = nvs_get_u8(handle, "short_min", &snapshot.settings.short_break_minutes) == ESP_OK;
    const bool has_long        = nvs_get_u8(handle, "long_min", &snapshot.settings.long_break_minutes) == ESP_OK;
    const bool has_interval    = nvs_get_u8(handle, "interval", &snapshot.settings.long_break_interval) == ESP_OK;
    snapshot.settings_valid    = snapshot.schema_valid && has_focus && has_short && has_long && has_interval
                                 && settings_are_valid(&snapshot.settings);

    const esp_err_t date_error = nvs_get_u32(handle, "today_date", &snapshot.today_date);
    if (date_error == ESP_ERR_NVS_NOT_FOUND)
    {
        snapshot.today_date = 0U;
    }
    const esp_err_t today_error = nvs_get_u8(handle, "today_count", &snapshot.today_count);
    if (today_error == ESP_ERR_NVS_NOT_FOUND)
    {
        snapshot.today_count = 0U;
    }
    const esp_err_t pending_error = nvs_get_u8(handle, "pending_count", &snapshot.pending_count);
    if (pending_error == ESP_ERR_NVS_NOT_FOUND)
    {
        snapshot.pending_count = 0U;
    }
    snapshot.counts_valid = (date_error == ESP_OK || date_error == ESP_ERR_NVS_NOT_FOUND)
                            && (today_error == ESP_OK || today_error == ESP_ERR_NVS_NOT_FOUND)
                            && (pending_error == ESP_OK || pending_error == ESP_ERR_NVS_NOT_FOUND);
    nvs_close(handle);
    *out_snapshot = snapshot;
    return ESP_OK;
}

esp_err_t pomodoro_store_save_settings_copy(const pomodoro_store_settings_t *settings)
{
    if (!settings_are_valid(settings))
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t    error = open_store(NVS_READWRITE, &handle);
    if (error != ESP_OK)
    {
        return error;
    }
    error = nvs_set_u8(handle, "schema_ver", POMODORO_STORE_SCHEMA_VERSION);
    if (error == ESP_OK)
    {
        error = nvs_set_u8(handle, "focus_min", settings->focus_minutes);
    }
    if (error == ESP_OK)
    {
        error = nvs_set_u8(handle, "short_min", settings->short_break_minutes);
    }
    if (error == ESP_OK)
    {
        error = nvs_set_u8(handle, "long_min", settings->long_break_minutes);
    }
    if (error == ESP_OK)
    {
        error = nvs_set_u8(handle, "interval", settings->long_break_interval);
    }
    if (error == ESP_OK)
    {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t pomodoro_store_save_counts(uint32_t today_date, uint8_t today_count, uint8_t pending_count)
{
    nvs_handle_t handle;
    esp_err_t    error = open_store(NVS_READWRITE, &handle);
    if (error != ESP_OK)
    {
        return error;
    }
    error = nvs_set_u32(handle, "today_date", today_date);
    if (error == ESP_OK)
    {
        error = nvs_set_u8(handle, "today_count", today_count);
    }
    if (error == ESP_OK)
    {
        error = nvs_set_u8(handle, "pending_count", pending_count);
    }
    if (error == ESP_OK)
    {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}
