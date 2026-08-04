/**
 * @file pomodoro_store.c
 * @brief 使用独立 NVS 命名空间保存番茄钟设置和完成计数
 */
#include "pomodoro_store.h"

#include <string.h>

#include "nvs.h"

#define POMODORO_NAMESPACE             "pomodoro"
#define POMODORO_LEGACY_SCHEMA_VERSION 1U

_Static_assert(sizeof(POMODORO_STORE_DEFAULT_COMPLETION_AUDIO_PATH) - 1U
                   <= POMODORO_STORE_COMPLETION_AUDIO_PATH_MAX_LENGTH,
               "番茄钟默认完成音乐路径不得超过持久化上限");

/** @brief 校验一个 UTF-8 字节序列，不接受过长编码、代理项或超出 Unicode 的码点。 */
static bool utf8_is_valid(const char *text, size_t length)
{
    size_t index = 0U;
    while (index < length)
    {
        const uint8_t first = (uint8_t) text[index];
        if (first <= 0x7FU)
        {
            ++index;
            continue;
        }

        size_t sequence_length = 0U;
        if (first >= 0xC2U && first <= 0xDFU)
        {
            sequence_length = 2U;
        }
        else if (first >= 0xE0U && first <= 0xEFU)
        {
            sequence_length = 3U;
        }
        else if (first >= 0xF0U && first <= 0xF4U)
        {
            sequence_length = 4U;
        }
        else
        {
            return false;
        }
        if (sequence_length > length - index)
        {
            return false;
        }

        const uint8_t second = (uint8_t) text[index + 1U];
        if ((second & 0xC0U) != 0x80U
            || (first == 0xE0U && second < 0xA0U)
            || (first == 0xEDU && second > 0x9FU)
            || (first == 0xF0U && second < 0x90U)
            || (first == 0xF4U && second > 0x8FU))
        {
            return false;
        }
        for (size_t offset = 2U; offset < sequence_length; ++offset)
        {
            if (((uint8_t) text[index + offset] & 0xC0U) != 0x80U)
            {
                return false;
            }
        }
        index += sequence_length;
    }
    return true;
}

/** @brief 校验一个路径段是否为禁止的 `.` 或 `..`。 */
static bool path_segment_is_reserved(const char *path, size_t start, size_t length)
{
    return (length == 1U && path[start] == '.') || (length == 2U && path[start] == '.' && path[start + 1U] == '.');
}

/** @brief 校验持久化完成音乐使用规范、相对挂载点的 MP3 逻辑路径。 */
static bool completion_audio_path_is_valid(const char *path)
{
    if (path == NULL)
    {
        return false;
    }
    const size_t length = strnlen(path, POMODORO_STORE_COMPLETION_AUDIO_PATH_MAX_LENGTH + 1U);
    if (length <= 4U || length > POMODORO_STORE_COMPLETION_AUDIO_PATH_MAX_LENGTH || path[0] != '/'
        || !utf8_is_valid(path, length))
    {
        return false;
    }

    size_t segment_start = 1U;
    for (size_t index = 1U; index < length; ++index)
    {
        const unsigned char value = (unsigned char) path[index];
        if (value < 0x20U || value == 0x7FU || value == '\\')
        {
            return false;
        }
        if (value == '/')
        {
            const size_t segment_length = index - segment_start;
            if (segment_length == 0U || path_segment_is_reserved(path, segment_start, segment_length))
            {
                return false;
            }
            segment_start = index + 1U;
        }
    }
    const size_t final_segment_length = length - segment_start;
    if (final_segment_length == 0U || path_segment_is_reserved(path, segment_start, final_segment_length))
    {
        return false;
    }

    const char *suffix = &path[length - 4U];
    return suffix[0] == '.' && (suffix[1] == 'm' || suffix[1] == 'M') && (suffix[2] == 'p' || suffix[2] == 'P')
           && suffix[3] == '3';
}

bool pomodoro_store_settings_are_valid(const pomodoro_store_settings_t *settings)
{
    return settings != NULL && settings->focus_minutes >= 5U && settings->focus_minutes <= 90U
           && (settings->focus_minutes % 5U) == 0U && settings->short_break_minutes >= 1U
           && settings->short_break_minutes <= 30U && settings->long_break_minutes >= 5U
           && settings->long_break_minutes <= 60U && (settings->long_break_minutes % 5U) == 0U
           && settings->long_break_interval >= 2U && settings->long_break_interval <= 8U
           && completion_audio_path_is_valid(settings->completion_audio_path);
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

    uint8_t         schema       = 0U;
    const esp_err_t schema_error = nvs_get_u8(handle, "schema_ver", &schema);
    snapshot.schema_valid        = schema_error == ESP_OK && schema == POMODORO_STORE_SCHEMA_VERSION;
    snapshot.migration_required  = schema_error == ESP_OK && schema == POMODORO_LEGACY_SCHEMA_VERSION;
    const bool recognized_schema = snapshot.schema_valid || snapshot.migration_required;
    const bool has_focus         = nvs_get_u8(handle, "focus_min", &snapshot.settings.focus_minutes) == ESP_OK;
    const bool has_short         = nvs_get_u8(handle, "short_min", &snapshot.settings.short_break_minutes) == ESP_OK;
    const bool has_long          = nvs_get_u8(handle, "long_min", &snapshot.settings.long_break_minutes) == ESP_OK;
    const bool has_interval      = nvs_get_u8(handle, "interval", &snapshot.settings.long_break_interval) == ESP_OK;
    bool       has_audio_path    = false;
    if (snapshot.migration_required)
    {
        memcpy(snapshot.settings.completion_audio_path,
               POMODORO_STORE_DEFAULT_COMPLETION_AUDIO_PATH,
               sizeof(POMODORO_STORE_DEFAULT_COMPLETION_AUDIO_PATH));
        has_audio_path = true;
    }
    else if (snapshot.schema_valid)
    {
        size_t audio_path_size = sizeof(snapshot.settings.completion_audio_path);
        has_audio_path =
            nvs_get_str(handle, "complete_mp3", snapshot.settings.completion_audio_path, &audio_path_size) == ESP_OK;
    }
    snapshot.settings_valid = recognized_schema && has_focus && has_short && has_long && has_interval && has_audio_path
                              && pomodoro_store_settings_are_valid(&snapshot.settings);

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
    if (!pomodoro_store_settings_are_valid(settings))
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
        error = nvs_set_str(handle, "complete_mp3", settings->completion_audio_path);
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
