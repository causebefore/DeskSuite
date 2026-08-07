/**
 * @file app_web_console_provider.c
 * @brief 把 DeskMate Hub、番茄钟设置与系统事实映射为网页控制台 Provider
 */
#include "app_web_console_provider.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_WEB_CONSOLE_SETTINGS || CONFIG_WEB_CONSOLE_ACTIONS
#include "app_network.h"
#endif

#if CONFIG_WEB_CONSOLE_SETTINGS
#include "app_pomodoro.h"
#endif

#if CONFIG_WEB_CONSOLE_ACTIONS

typedef enum
{
    HUB_ACTION_TEST_CONNECTION = 0,
    HUB_ACTION_COUNT,
} hub_action_index_t;

typedef enum
{
    HUB_ACTION_FIELD_URL = 0,
    HUB_ACTION_FIELD_COUNT,
} hub_action_field_index_t;

static const web_console_field_info_t s_hub_action_fields[HUB_ACTION_FIELD_COUNT] = {
    [HUB_ACTION_FIELD_URL] =
        {
            .id               = "hub_url",
            .label            = "Hub 地址",
            .description      = "要由设备测试的完整 LAN HTTP 地址。",
            .format           = "url",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = 0U,
            .max_length_bytes = APP_NETWORK_HUB_URL_MAX_LENGTH,
        },
};

static const web_console_action_info_t s_hub_actions[HUB_ACTION_COUNT] = {
    [HUB_ACTION_TEST_CONNECTION] =
        {
            .id                = "test_connection",
            .label             = "测试连接",
            .description       = "由设备对候选 Hub 的无凭据健康接口执行一次有界测试。",
            .input_fields      = s_hub_action_fields,
            .input_field_count = HUB_ACTION_FIELD_COUNT,
        },
};

/** @brief 校验并规范化 Hub 测试 Action 的唯一地址输入。 */
static esp_err_t get_hub_url_from_action_request(
    const web_console_action_request_t *action_request,
    char out_url[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U])
{
    if (action_request == NULL || out_url == NULL || action_request->action_index != HUB_ACTION_TEST_CONNECTION
        || action_request->inputs == NULL || action_request->input_count != 1U
        || action_request->inputs[0].field_index != HUB_ACTION_FIELD_URL
        || action_request->inputs[0].value.type != WEB_CONSOLE_FIELD_TYPE_STRING
        || !action_request->inputs[0].value.configured)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return app_network_hub_url_parse_copy(action_request->inputs[0].value.data.string_value, out_url);
}

/** @brief 无副作用校验 Hub 连接测试 Action。 */
static esp_err_t validate_hub_action_request(
    void *context,
    const web_console_action_request_t *action_request)
{
    (void) context;
    char normalized[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U];
    return get_hub_url_from_action_request(action_request, normalized);
}

/** @brief 把 Hub 连接测试 Action 复制到 Network Application 单 pending 槽。 */
static esp_err_t request_hub_action_copy(
    void *context,
    const web_console_action_request_t *action_request,
    uint64_t *out_request_id)
{
    (void) context;
    char normalized[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U];
    const esp_err_t parse_error = get_hub_url_from_action_request(action_request, normalized);
    return parse_error == ESP_OK ? app_network_request_test_hub_url_copy(normalized, out_request_id) : parse_error;
}

/** @brief 把 Network Application 稳定原因映射为 Console Action 稳定原因。 */
static web_console_result_reason_t map_hub_result_reason(app_network_hub_result_reason_t reason)
{
    switch (reason)
    {
        case APP_NETWORK_HUB_RESULT_REASON_NONE:
            return WEB_CONSOLE_RESULT_REASON_NONE;
        case APP_NETWORK_HUB_RESULT_REASON_VERSION_CONFLICT:
            return WEB_CONSOLE_RESULT_REASON_VERSION_CONFLICT;
        case APP_NETWORK_HUB_RESULT_REASON_OWNER_BUSY:
            return WEB_CONSOLE_RESULT_REASON_OWNER_BUSY;
        case APP_NETWORK_HUB_RESULT_REASON_VALIDATION_FAILED:
            return WEB_CONSOLE_RESULT_REASON_VALIDATION_FAILED;
        case APP_NETWORK_HUB_RESULT_REASON_PERSISTENCE_FAILED:
            return WEB_CONSOLE_RESULT_REASON_PERSISTENCE_FAILED;
        case APP_NETWORK_HUB_RESULT_REASON_CONNECTION_FAILED:
            return WEB_CONSOLE_RESULT_REASON_CONNECTION_FAILED;
        case APP_NETWORK_HUB_RESULT_REASON_HEALTH_CHECK_FAILED:
            return WEB_CONSOLE_RESULT_REASON_HEALTH_CHECK_FAILED;
        case APP_NETWORK_HUB_RESULT_REASON_TIMEOUT:
            return WEB_CONSOLE_RESULT_REASON_TIMEOUT;
        case APP_NETWORK_HUB_RESULT_REASON_UNKNOWN:
        default:
            return WEB_CONSOLE_RESULT_REASON_UNKNOWN;
    }
}

/** @brief 把 Network Application 测试结果映射为 Console Action 结果。 */
static esp_err_t get_hub_action_result_copy(
    void *context,
    size_t action_index,
    uint64_t request_id,
    web_console_action_result_t *out_result)
{
    (void) context;
    if (action_index != HUB_ACTION_TEST_CONNECTION || out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    app_network_hub_request_result_t result;
    const esp_err_t error = app_network_get_hub_request_result_copy(request_id, &result);
    if (error != ESP_OK)
    {
        return error;
    }
    if (result.operation != APP_NETWORK_HUB_OPERATION_TEST)
    {
        return ESP_ERR_NOT_FOUND;
    }
    switch (result.state)
    {
        case APP_NETWORK_HUB_REQUEST_STATE_PENDING:
            out_result->state = WEB_CONSOLE_ACTION_STATE_PENDING;
            break;
        case APP_NETWORK_HUB_REQUEST_STATE_SUCCEEDED:
            out_result->state = WEB_CONSOLE_ACTION_STATE_SUCCEEDED;
            break;
        case APP_NETWORK_HUB_REQUEST_STATE_FAILED:
            out_result->state = WEB_CONSOLE_ACTION_STATE_FAILED;
            break;
        default:
            return ESP_ERR_INVALID_STATE;
    }
    out_result->reason = map_hub_result_reason(result.reason);
    return ESP_OK;
}

static const web_console_action_provider_t s_action_providers[] = {
    {
        .section_id       = "hub",
        .label            = "Hub",
        .description      = "测试候选 DeskSuite Hub，不会修改当前设置。",
        .actions          = s_hub_actions,
        .action_count     = HUB_ACTION_COUNT,
        .validate_request = validate_hub_action_request,
        .request_copy     = request_hub_action_copy,
        .get_result_copy  = get_hub_action_result_copy,
        .context          = NULL,
    },
};

#endif

#if CONFIG_WEB_CONSOLE_STATUS
#include "system_info.h"
#endif

#if CONFIG_WEB_CONSOLE_SETTINGS

typedef enum
{
    HUB_FIELD_URL = 0,
    HUB_FIELD_COUNT,
} hub_field_index_t;

static const web_console_field_info_t s_hub_fields[HUB_FIELD_COUNT] = {
    [HUB_FIELD_URL] =
        {
            .id               = "hub_url",
            .label            = "Hub 地址",
            .description      = "设备将通过该地址获取天气、日历、邮件、额度和语音服务。",
            .summary          = "Hub",
            .format           = "url",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = 0U,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NEXT_TRANSACTION,
            .max_length_bytes = APP_NETWORK_HUB_URL_MAX_LENGTH,
        },
};

/** @brief 校验并复制 Hub Settings 的唯一地址字段。 */
static esp_err_t get_hub_url_from_settings_update(
    const web_console_settings_update_t *update,
    char out_url[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U])
{
    if (update == NULL || out_url == NULL || update->fields == NULL || update->field_count != 1U
        || update->fields[0].field_index != HUB_FIELD_URL
        || update->fields[0].value.type != WEB_CONSOLE_FIELD_TYPE_STRING
        || !update->fields[0].value.configured)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return app_network_hub_url_parse_copy(update->fields[0].value.data.string_value, out_url);
}

/** @brief 复制 Hub 地址与 Network Application 设置版本。 */
static esp_err_t get_hub_settings_snapshot_copy(
    void *context,
    web_console_settings_snapshot_t *out_snapshot)
{
    (void) context;
    if (out_snapshot == NULL || out_snapshot->values == NULL || out_snapshot->value_capacity < HUB_FIELD_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    app_network_hub_settings_snapshot_t snapshot;
    const esp_err_t error = app_network_get_hub_settings_snapshot_copy(&snapshot);
    if (error != ESP_OK)
    {
        return error;
    }
    memset(&out_snapshot->values[HUB_FIELD_URL], 0, sizeof(out_snapshot->values[HUB_FIELD_URL]));
    out_snapshot->values[HUB_FIELD_URL].type       = WEB_CONSOLE_FIELD_TYPE_STRING;
    out_snapshot->values[HUB_FIELD_URL].configured = true;
    (void) snprintf(out_snapshot->values[HUB_FIELD_URL].data.string_value,
                    sizeof(out_snapshot->values[HUB_FIELD_URL].data.string_value),
                    "%s",
                    snapshot.service_url);
    out_snapshot->version     = snapshot.version;
    out_snapshot->value_count = HUB_FIELD_COUNT;
    return ESP_OK;
}

/** @brief 无副作用校验 Hub 地址更新及其期望版本。 */
static esp_err_t validate_hub_settings_update(
    void *context,
    const web_console_settings_update_t *update)
{
    (void) context;
    char normalized[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U];
    const esp_err_t parse_error = get_hub_url_from_settings_update(update, normalized);
    if (parse_error != ESP_OK)
    {
        return parse_error;
    }
    app_network_hub_settings_snapshot_t snapshot;
    const esp_err_t snapshot_error = app_network_get_hub_settings_snapshot_copy(&snapshot);
    if (snapshot_error != ESP_OK)
    {
        return snapshot_error;
    }
    return snapshot.version == update->expected_version ? ESP_OK : ESP_ERR_INVALID_VERSION;
}

/** @brief 把 Hub Settings 更新复制到 Network Application 单 pending 槽。 */
static esp_err_t request_hub_settings_update_copy(
    void *context,
    const web_console_settings_update_t *update,
    uint64_t *out_request_id)
{
    (void) context;
    char normalized[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U];
    const esp_err_t parse_error = get_hub_url_from_settings_update(update, normalized);
    return parse_error == ESP_OK
               ? app_network_request_update_hub_url_copy(normalized, update->expected_version, out_request_id)
               : parse_error;
}

/** @brief 把 Network Application 更新结果映射为 Console Settings 结果。 */
static esp_err_t get_hub_settings_update_result_copy(
    void *context,
    uint64_t request_id,
    web_console_settings_update_result_t *out_result)
{
    (void) context;
    if (out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    app_network_hub_request_result_t result;
    const esp_err_t error = app_network_get_hub_request_result_copy(request_id, &result);
    if (error != ESP_OK)
    {
        return error;
    }
    if (result.operation != APP_NETWORK_HUB_OPERATION_UPDATE)
    {
        return ESP_ERR_NOT_FOUND;
    }
    switch (result.state)
    {
        case APP_NETWORK_HUB_REQUEST_STATE_PENDING:
            out_result->state = WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING;
            break;
        case APP_NETWORK_HUB_REQUEST_STATE_SUCCEEDED:
            out_result->state = WEB_CONSOLE_SETTINGS_UPDATE_STATE_SUCCEEDED;
            break;
        case APP_NETWORK_HUB_REQUEST_STATE_FAILED:
            out_result->state = WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED;
            break;
        default:
            return ESP_ERR_INVALID_STATE;
    }
    out_result->version = result.version;
    out_result->error   = result.error;
    return ESP_OK;
}

typedef enum
{
    POMODORO_FIELD_FOCUS_MINUTES = 0,
    POMODORO_FIELD_SHORT_BREAK_MINUTES,
    POMODORO_FIELD_LONG_BREAK_MINUTES,
    POMODORO_FIELD_LONG_BREAK_INTERVAL,
    POMODORO_FIELD_COMPLETION_AUDIO_PATH,
    POMODORO_FIELD_COUNT,
} pomodoro_field_index_t;

static const web_console_field_info_t s_pomodoro_fields[POMODORO_FIELD_COUNT] = {
    [POMODORO_FIELD_FOCUS_MINUTES] =
        {
            .id      = "focus_minutes",
            .label   = "专注时长",
            .description = "每轮专注阶段持续的分钟数。",
            .unit    = "分钟",
            .summary = "专注",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = 0U,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .minimum = 5,
            .maximum = 180,
            .step    = 1U,
        },
    [POMODORO_FIELD_SHORT_BREAK_MINUTES] =
        {
            .id      = "short_break_minutes",
            .label   = "短休时长",
            .description = "普通专注轮次后的休息分钟数。",
            .unit    = "分钟",
            .summary = "短休",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = 0U,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .minimum = 5,
            .maximum = 180,
            .step    = 1U,
        },
    [POMODORO_FIELD_LONG_BREAK_MINUTES] =
        {
            .id      = "long_break_minutes",
            .label   = "长休时长",
            .description = "完成指定轮次后长休阶段的分钟数。",
            .unit    = "分钟",
            .summary = "长休",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = 0U,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .minimum = 5,
            .maximum = 180,
            .step    = 1U,
        },
    [POMODORO_FIELD_LONG_BREAK_INTERVAL] =
        {
            .id      = "long_break_interval",
            .label   = "长休间隔",
            .description = "每完成多少轮专注后进入一次长休。",
            .unit    = "轮",
            .summary = "长休间隔",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = 0U,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .minimum = 2,
            .maximum = 12,
            .step    = 1U,
        },
    [POMODORO_FIELD_COMPLETION_AUDIO_PATH] =
        {
            .id               = "completion_audio_path",
            .label            = "完成音乐",
            .description      = "阶段完成时由 Audio Service 播放的 SD 卡 MP3 文件。",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = 0U,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY,
            .max_length_bytes = APP_POMODORO_COMPLETION_AUDIO_PATH_MAX_LENGTH,
            .file_suffix      = ".mp3",
        },
};

/** @brief 把 Pomodoro 设置字段写入 Console 按描述符排序的值数组。 */
static void write_pomodoro_values(
    const app_pomodoro_settings_t *settings,
    web_console_field_value_t values[POMODORO_FIELD_COUNT])
{
    memset(values, 0, sizeof(*values) * POMODORO_FIELD_COUNT);
    for (size_t index = 0U; index < POMODORO_FIELD_COMPLETION_AUDIO_PATH; ++index)
    {
        values[index].type       = WEB_CONSOLE_FIELD_TYPE_UINT32;
        values[index].configured = true;
    }
    values[POMODORO_FIELD_FOCUS_MINUTES].data.uint32_value       = settings->focus_minutes;
    values[POMODORO_FIELD_SHORT_BREAK_MINUTES].data.uint32_value = settings->short_break_minutes;
    values[POMODORO_FIELD_LONG_BREAK_MINUTES].data.uint32_value  = settings->long_break_minutes;
    values[POMODORO_FIELD_LONG_BREAK_INTERVAL].data.uint32_value = settings->long_break_interval;
    values[POMODORO_FIELD_COMPLETION_AUDIO_PATH].type             = WEB_CONSOLE_FIELD_TYPE_STRING;
    values[POMODORO_FIELD_COMPLETION_AUDIO_PATH].configured       = true;
    (void) snprintf(values[POMODORO_FIELD_COMPLETION_AUDIO_PATH].data.string_value,
                    sizeof(values[POMODORO_FIELD_COMPLETION_AUDIO_PATH].data.string_value),
                    "%s",
                    settings->completion_audio_path);
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
            || !field->value.configured)
        {
            return ESP_ERR_INVALID_ARG;
        }
        seen[field->field_index] = true;
        if (field->field_index == POMODORO_FIELD_COMPLETION_AUDIO_PATH)
        {
            const size_t path_length = strnlen(field->value.data.string_value,
                                               APP_POMODORO_COMPLETION_AUDIO_PATH_MAX_LENGTH + 1U);
            if (field->value.type != WEB_CONSOLE_FIELD_TYPE_STRING
                || path_length > APP_POMODORO_COMPLETION_AUDIO_PATH_MAX_LENGTH)
            {
                return ESP_ERR_INVALID_ARG;
            }
            memcpy(out_update->settings.completion_audio_path,
                   field->value.data.string_value,
                   path_length + 1U);
            continue;
        }
        if (field->value.type != WEB_CONSOLE_FIELD_TYPE_UINT32
            || field->value.data.uint32_value > UINT8_MAX)
        {
            return ESP_ERR_INVALID_ARG;
        }
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
            case POMODORO_FIELD_COMPLETION_AUDIO_PATH:
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
        .section_id             = "hub",
        .label                  = "Hub",
        .description            = "配置 DeskSuite Hub，并在保存前由设备验证连接。",
        .fields                 = s_hub_fields,
        .field_count            = HUB_FIELD_COUNT,
        .get_snapshot_copy      = get_hub_settings_snapshot_copy,
        .validate_update        = validate_hub_settings_update,
        .request_update_copy    = request_hub_settings_update_copy,
        .get_update_result_copy = get_hub_settings_update_result_copy,
        .context                = NULL,
    },
    {
        .section_id            = "pomodoro",
        .label                 = "番茄钟",
        .description           = "调整专注节奏和阶段完成音乐；运行时保持只读。",
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
        .label           = "设备与系统",
        .description     = "查看设备固件、运行时长、资源和上次重启原因。",
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

const web_console_action_provider_t *app_web_console_provider_get_actions_borrow(size_t *out_count)
{
    if (out_count == NULL)
    {
        return NULL;
    }
#if CONFIG_WEB_CONSOLE_ACTIONS
    *out_count = sizeof(s_action_providers) / sizeof(s_action_providers[0]);
    return s_action_providers;
#else
    *out_count = 0U;
    return NULL;
#endif
}
