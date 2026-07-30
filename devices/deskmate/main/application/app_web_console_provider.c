/**
 * @file app_web_console_provider.c
 * @brief 把 DeskMate 番茄钟设置与系统事实映射为网页控制台 Provider
 */
#include "app_web_console_provider.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_WEB_CONSOLE_SETTINGS
#include "app_pomodoro.h"
#endif

#if CONFIG_WEB_CONSOLE_STATUS
#include "system_info.h"
#endif

#if CONFIG_WEB_CONSOLE_SETTINGS

typedef enum
{
    POMODORO_FIELD_FOCUS_MINUTES = 0,
    POMODORO_FIELD_SHORT_BREAK_MINUTES,
    POMODORO_FIELD_LONG_BREAK_MINUTES,
    POMODORO_FIELD_LONG_BREAK_INTERVAL,
    POMODORO_FIELD_COUNT,
} pomodoro_field_index_t;

static const web_console_field_info_t s_pomodoro_fields[POMODORO_FIELD_COUNT] = {
    [POMODORO_FIELD_FOCUS_MINUTES] =
        {
            .id      = "focus_minutes",
            .label   = "专注时长（分钟）",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = 0U,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .minimum = 5,
            .maximum = 90,
            .step    = 5U,
        },
    [POMODORO_FIELD_SHORT_BREAK_MINUTES] =
        {
            .id      = "short_break_minutes",
            .label   = "短休时长（分钟）",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = 0U,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .minimum = 1,
            .maximum = 30,
            .step    = 1U,
        },
    [POMODORO_FIELD_LONG_BREAK_MINUTES] =
        {
            .id      = "long_break_minutes",
            .label   = "长休时长（分钟）",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = 0U,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .minimum = 5,
            .maximum = 60,
            .step    = 5U,
        },
    [POMODORO_FIELD_LONG_BREAK_INTERVAL] =
        {
            .id      = "long_break_interval",
            .label   = "长休间隔（轮）",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = 0U,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .minimum = 2,
            .maximum = 8,
            .step    = 1U,
        },
};

/** @brief 把 Pomodoro 设置字段写入 Console 按描述符排序的值数组。 */
static void write_pomodoro_values(
    const app_pomodoro_settings_t *settings,
    web_console_field_value_t values[POMODORO_FIELD_COUNT])
{
    memset(values, 0, sizeof(*values) * POMODORO_FIELD_COUNT);
    for (size_t index = 0U; index < POMODORO_FIELD_COUNT; ++index)
    {
        values[index].type       = WEB_CONSOLE_FIELD_TYPE_UINT32;
        values[index].configured = true;
    }
    values[POMODORO_FIELD_FOCUS_MINUTES].data.uint32_value       = settings->focus_minutes;
    values[POMODORO_FIELD_SHORT_BREAK_MINUTES].data.uint32_value = settings->short_break_minutes;
    values[POMODORO_FIELD_LONG_BREAK_MINUTES].data.uint32_value  = settings->long_break_minutes;
    values[POMODORO_FIELD_LONG_BREAK_INTERVAL].data.uint32_value = settings->long_break_interval;
}

/**
 * @brief 基于所有者最新完整设置合并一份 Console 稀疏更新
 *
 * 本函数只读取一次 Pomodoro 快照并复制字段，不保存 Console 输入指针。版本仍由所有者
 * validate/request API 在各自接受点重新校验。
 */
static esp_err_t make_pomodoro_settings_update(
    const web_console_settings_update_t *update,
    app_pomodoro_settings_update_t *out_update)
{
    if (update == NULL || out_update == NULL || update->fields == NULL || update->field_count == 0U
        || update->field_count > POMODORO_FIELD_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_pomodoro_snapshot_t snapshot;
    const esp_err_t         snapshot_error = app_pomodoro_get_snapshot_copy(&snapshot);
    if (snapshot_error != ESP_OK)
    {
        return snapshot_error;
    }
    if (snapshot.settings_version != update->expected_version)
    {
        return ESP_ERR_INVALID_VERSION;
    }

    *out_update = (app_pomodoro_settings_update_t) {
        .settings         = snapshot.settings,
        .expected_version = update->expected_version,
    };
    bool seen[POMODORO_FIELD_COUNT] = { false };
    for (size_t index = 0U; index < update->field_count; ++index)
    {
        const web_console_settings_update_field_t *field = &update->fields[index];
        if (field->field_index >= POMODORO_FIELD_COUNT || seen[field->field_index]
            || field->value.type != WEB_CONSOLE_FIELD_TYPE_UINT32 || !field->value.configured
            || field->value.data.uint32_value > UINT8_MAX)
        {
            return ESP_ERR_INVALID_ARG;
        }
        seen[field->field_index] = true;
        const uint8_t value      = (uint8_t) field->value.data.uint32_value;
        switch ((pomodoro_field_index_t) field->field_index)
        {
            case POMODORO_FIELD_FOCUS_MINUTES:
                out_update->settings.focus_minutes = value;
                break;
            case POMODORO_FIELD_SHORT_BREAK_MINUTES:
                out_update->settings.short_break_minutes = value;
                break;
            case POMODORO_FIELD_LONG_BREAK_MINUTES:
                out_update->settings.long_break_minutes = value;
                break;
            case POMODORO_FIELD_LONG_BREAK_INTERVAL:
                out_update->settings.long_break_interval = value;
                break;
            case POMODORO_FIELD_COUNT:
            default:
                return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

/** @brief 复制 Pomodoro 当前完整设置和独立设置版本。 */
static esp_err_t get_pomodoro_settings_snapshot_copy(
    void *context,
    web_console_settings_snapshot_t *out_snapshot)
{
    (void) context;
    if (out_snapshot == NULL || out_snapshot->values == NULL
        || out_snapshot->value_capacity < POMODORO_FIELD_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_pomodoro_snapshot_t snapshot;
    const esp_err_t         error = app_pomodoro_get_snapshot_copy(&snapshot);
    if (error != ESP_OK)
    {
        return error;
    }
    write_pomodoro_values(&snapshot.settings, out_snapshot->values);
    out_snapshot->version     = snapshot.settings_version;
    out_snapshot->value_count = POMODORO_FIELD_COUNT;
    return ESP_OK;
}

/** @brief 把 Console 稀疏更新交给 Pomodoro 所有者做无副作用领域校验。 */
static esp_err_t validate_pomodoro_settings_update(
    void *context,
    const web_console_settings_update_t *update)
{
    (void) context;
    app_pomodoro_settings_update_t owner_update;
    const esp_err_t                error = make_pomodoro_settings_update(update, &owner_update);
    return error == ESP_OK ? app_pomodoro_validate_settings_update(&owner_update) : error;
}

/** @brief 把 Console 稀疏更新复制为 Pomodoro 所有者的版本化异步请求。 */
static esp_err_t request_pomodoro_settings_update_copy(
    void *context,
    const web_console_settings_update_t *update,
    uint64_t *out_request_id)
{
    (void) context;
    app_pomodoro_settings_update_t owner_update;
    const esp_err_t                error = make_pomodoro_settings_update(update, &owner_update);
    return error == ESP_OK ? app_pomodoro_request_update_settings_copy(&owner_update, out_request_id) : error;
}

/** @brief 把 Pomodoro 所有者请求结果映射为 Console 通用结果。 */
static esp_err_t get_pomodoro_settings_update_result_copy(
    void *context,
    uint64_t request_id,
    web_console_settings_update_result_t *out_result)
{
    (void) context;
    if (out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_pomodoro_settings_update_result_t result;
    const esp_err_t                       error =
        app_pomodoro_get_settings_update_result_copy(request_id, &result);
    if (error != ESP_OK)
    {
        return error;
    }
    switch (result.state)
    {
        case APP_POMODORO_SETTINGS_UPDATE_STATE_PENDING:
            out_result->state = WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING;
            break;
        case APP_POMODORO_SETTINGS_UPDATE_STATE_SUCCEEDED:
            out_result->state = WEB_CONSOLE_SETTINGS_UPDATE_STATE_SUCCEEDED;
            break;
        case APP_POMODORO_SETTINGS_UPDATE_STATE_FAILED:
            out_result->state = WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED;
            break;
        default:
            return ESP_ERR_INVALID_STATE;
    }
    out_result->version = result.version;
    out_result->error   = result.error;
    return ESP_OK;
}

static const web_console_settings_provider_t s_settings_providers[] = {
    {
        .section_id            = "pomodoro",
        .label                 = "番茄钟",
        .fields                = s_pomodoro_fields,
        .field_count           = POMODORO_FIELD_COUNT,
        .get_snapshot_copy     = get_pomodoro_settings_snapshot_copy,
        .validate_update       = validate_pomodoro_settings_update,
        .request_update_copy   = request_pomodoro_settings_update_copy,
        .get_update_result_copy = get_pomodoro_settings_update_result_copy,
        .context               = NULL,
    },
};

#endif

#if CONFIG_WEB_CONSOLE_STATUS

typedef enum
{
    SYSTEM_STATUS_FIELD_FIRMWARE_VERSION = 0,
    SYSTEM_STATUS_FIELD_BUILD_TIME,
    SYSTEM_STATUS_FIELD_UPTIME_SEC,
    SYSTEM_STATUS_FIELD_SRAM_FREE_KB,
    SYSTEM_STATUS_FIELD_PSRAM_FREE_KB,
    SYSTEM_STATUS_FIELD_CPU_MHZ,
    SYSTEM_STATUS_FIELD_RESET_REASON,
    SYSTEM_STATUS_FIELD_COUNT,
} system_status_field_index_t;

#define SYSTEM_STATUS_READ_ONLY WEB_CONSOLE_FIELD_ACCESS_READ_ONLY

static const web_console_field_info_t s_system_status_fields[SYSTEM_STATUS_FIELD_COUNT] = {
    [SYSTEM_STATUS_FIELD_FIRMWARE_VERSION] =
        {
            .id               = "firmware_version",
            .label            = "固件版本",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = SYSTEM_STATUS_READ_ONLY,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .max_length_bytes = 31U,
        },
    [SYSTEM_STATUS_FIELD_BUILD_TIME] =
        {
            .id               = "build_time",
            .label            = "构建时间",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = SYSTEM_STATUS_READ_ONLY,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .max_length_bytes = 23U,
        },
    [SYSTEM_STATUS_FIELD_UPTIME_SEC] =
        {
            .id      = "uptime_sec",
            .label   = "运行时长（秒）",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = SYSTEM_STATUS_READ_ONLY,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .minimum = 0,
            .maximum = UINT32_MAX,
            .step    = 1U,
        },
    [SYSTEM_STATUS_FIELD_SRAM_FREE_KB] =
        {
            .id      = "sram_free_kb",
            .label   = "可用 SRAM（KiB）",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = SYSTEM_STATUS_READ_ONLY,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .minimum = 0,
            .maximum = UINT32_MAX,
            .step    = 1U,
        },
    [SYSTEM_STATUS_FIELD_PSRAM_FREE_KB] =
        {
            .id      = "psram_free_kb",
            .label   = "可用 PSRAM（KiB）",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = SYSTEM_STATUS_READ_ONLY,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .minimum = 0,
            .maximum = UINT32_MAX,
            .step    = 1U,
        },
    [SYSTEM_STATUS_FIELD_CPU_MHZ] =
        {
            .id      = "cpu_mhz",
            .label   = "CPU 频率（MHz）",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = SYSTEM_STATUS_READ_ONLY,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .minimum = 0,
            .maximum = UINT16_MAX,
            .step    = 1U,
        },
    [SYSTEM_STATUS_FIELD_RESET_REASON] =
        {
            .id               = "reset_reason",
            .label            = "重启原因",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = SYSTEM_STATUS_READ_ONLY,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .max_length_bytes = 15U,
        },
};

/** @brief 写入一个已配置的 Console 字符串字段。 */
static void write_string_value(web_console_field_value_t *out_value, const char *text)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type       = WEB_CONSOLE_FIELD_TYPE_STRING;
    out_value->configured = true;
    (void) snprintf(out_value->data.string_value, sizeof(out_value->data.string_value), "%s", text);
}

/** @brief 写入一个已配置的 Console uint32 字段。 */
static void write_uint32_value(web_console_field_value_t *out_value, uint32_t value)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type              = WEB_CONSOLE_FIELD_TYPE_UINT32;
    out_value->configured        = true;
    out_value->data.uint32_value = value;
}

/** @brief 采集一次现有 System 快照并映射为只读 Console 状态。 */
static esp_err_t get_system_status_copy(void *context, web_console_section_status_t *out_status)
{
    (void) context;
    if (out_status == NULL || out_status->values == NULL || out_status->value_capacity < SYSTEM_STATUS_FIELD_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    system_info_snapshot_t snapshot;
    const esp_err_t        error = system_info_get_snapshot_copy(&snapshot);
    if (error != ESP_OK)
    {
        return error;
    }
    if (!snapshot.valid)
    {
        return ESP_ERR_INVALID_STATE;
    }

    write_string_value(&out_status->values[SYSTEM_STATUS_FIELD_FIRMWARE_VERSION], snapshot.version);
    write_string_value(&out_status->values[SYSTEM_STATUS_FIELD_BUILD_TIME], snapshot.build_time);
    write_uint32_value(&out_status->values[SYSTEM_STATUS_FIELD_UPTIME_SEC], snapshot.uptime_sec);
    write_uint32_value(&out_status->values[SYSTEM_STATUS_FIELD_SRAM_FREE_KB], snapshot.sram_free_kb);
    write_uint32_value(&out_status->values[SYSTEM_STATUS_FIELD_PSRAM_FREE_KB], snapshot.psram_free_kb);
    write_uint32_value(&out_status->values[SYSTEM_STATUS_FIELD_CPU_MHZ], snapshot.cpu_mhz);
    write_string_value(&out_status->values[SYSTEM_STATUS_FIELD_RESET_REASON],
                       system_info_get_reset_reason_borrow());
    out_status->version     = 0U;
    out_status->value_count = SYSTEM_STATUS_FIELD_COUNT;
    return ESP_OK;
}

static const web_console_status_provider_t s_status_providers[] = {
    {
        .section_id      = "system",
        .label           = "系统状态",
        .fields          = s_system_status_fields,
        .field_count     = SYSTEM_STATUS_FIELD_COUNT,
        .get_status_copy = get_system_status_copy,
        .context         = NULL,
    },
};

#endif

const web_console_settings_provider_t *app_web_console_provider_get_settings_borrow(size_t *out_count)
{
    if (out_count == NULL)
    {
        return NULL;
    }
#if CONFIG_WEB_CONSOLE_SETTINGS
    *out_count = sizeof(s_settings_providers) / sizeof(s_settings_providers[0]);
    return s_settings_providers;
#else
    *out_count = 0U;
    return NULL;
#endif
}

const web_console_status_provider_t *app_web_console_provider_get_status_borrow(size_t *out_count)
{
    if (out_count == NULL)
    {
        return NULL;
    }
#if CONFIG_WEB_CONSOLE_STATUS
    *out_count = sizeof(s_status_providers) / sizeof(s_status_providers[0]);
    return s_status_providers;
#else
    *out_count = 0U;
    return NULL;
#endif
}
