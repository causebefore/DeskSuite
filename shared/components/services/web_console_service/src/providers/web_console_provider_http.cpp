/**
 * @file web_console_provider_http.cpp
 * @brief Settings/Status/Actions Provider 的认证 HTTP 映射与有界 JSON 编解码
 */
#include "web_console_provider_internal.hpp"

#include <cmath>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "web_console_http_common.hpp"

#define WEB_CONSOLE_PROVIDER_REQUEST_BODY_MAX_BYTES 2048U
#define WEB_CONSOLE_PROVIDER_QUERY_MAX_BYTES        159U
#define WEB_CONSOLE_PROVIDER_JSON_DEPTH_MAX         16U
#define WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES    4096U
#define WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE        21U

enum web_console_update_parse_result_t
{
    WEB_CONSOLE_UPDATE_PARSE_OK = 0,
    WEB_CONSOLE_UPDATE_PARSE_MALFORMED,
    WEB_CONSOLE_UPDATE_PARSE_INVALID_CHANGE,
    WEB_CONSOLE_UPDATE_PARSE_SECRET,
};

#if CONFIG_WEB_CONSOLE_ACTIONS
enum web_console_action_parse_result_t
{
    WEB_CONSOLE_ACTION_PARSE_OK = 0,
    WEB_CONSOLE_ACTION_PARSE_MALFORMED,
    WEB_CONSOLE_ACTION_PARSE_INVALID_INPUT,
    WEB_CONSOLE_ACTION_PARSE_SECRET,
};
#endif

/** @brief 以不会被编译器省略的逐字节写入清除有界工作区。 */
static void web_console_provider_secure_clear(void *data, size_t size)
{
    volatile uint8_t *cursor = static_cast<volatile uint8_t *>(data);
    while (size > 0U)
    {
        *cursor = 0U;
        ++cursor;
        --size;
    }
}

/** @brief 返回字段类型的稳定 JSON 名称。 */
static const char *web_console_field_type_name(web_console_field_type_t type)
{
    switch (type)
    {
        case WEB_CONSOLE_FIELD_TYPE_BOOL:
            return "bool";
        case WEB_CONSOLE_FIELD_TYPE_INT32:
            return "int32";
        case WEB_CONSOLE_FIELD_TYPE_UINT32:
            return "uint32";
        case WEB_CONSOLE_FIELD_TYPE_STRING:
            return "string";
        case WEB_CONSOLE_FIELD_TYPE_ENUM:
            return "enum";
        default:
            return NULL;
    }
}

/** @brief 返回设置生效事实的稳定 JSON 名称。 */
static const char *web_console_field_effect_name(web_console_field_effect_t effect)
{
    switch (effect)
    {
        case WEB_CONSOLE_FIELD_EFFECT_NONE:
            return "none";
        case WEB_CONSOLE_FIELD_EFFECT_IMMEDIATE:
            return "immediate";
        case WEB_CONSOLE_FIELD_EFFECT_NEXT_TRANSACTION:
            return "next_transaction";
        case WEB_CONSOLE_FIELD_EFFECT_RECONNECT:
            return "reconnect";
        case WEB_CONSOLE_FIELD_EFFECT_RESTART:
            return "restart";
        case WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY:
            return "idle_only";
        default:
            return NULL;
    }
}

#if CONFIG_WEB_CONSOLE_ACTIONS
/** @brief 返回稳定结果原因的 JSON 名称。 */
static const char *web_console_result_reason_name(web_console_result_reason_t reason)
{
    switch (reason)
    {
        case WEB_CONSOLE_RESULT_REASON_NONE:
            return "none";
        case WEB_CONSOLE_RESULT_REASON_VERSION_CONFLICT:
            return "version_conflict";
        case WEB_CONSOLE_RESULT_REASON_OWNER_BUSY:
            return "owner_busy";
        case WEB_CONSOLE_RESULT_REASON_VALIDATION_FAILED:
            return "validation_failed";
        case WEB_CONSOLE_RESULT_REASON_PERSISTENCE_FAILED:
            return "persistence_failed";
        case WEB_CONSOLE_RESULT_REASON_CONNECTION_FAILED:
            return "connection_failed";
        case WEB_CONSOLE_RESULT_REASON_HEALTH_CHECK_FAILED:
            return "health_check_failed";
        case WEB_CONSOLE_RESULT_REASON_TIMEOUT:
            return "timeout";
        case WEB_CONSOLE_RESULT_REASON_UNKNOWN:
            return "unknown";
        default:
            return NULL;
    }
}
#endif

/** @brief 发送 Provider 路由共用的固定错误。 */
static esp_err_t web_console_provider_send_error(httpd_req_t *request,
                                                 const char *status,
                                                 const char *body)
{
    return web_console_http_send_json_error(request, status, body);
}

/** @brief 完成普通 Bearer 认证，失败时直接发送固定错误。 */
static esp_err_t web_console_provider_authorize(httpd_req_t *request, bool *out_authorized)
{
    if (out_authorized == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const web_console_http_auth_result_t result = web_console_http_authorize_request(request);
    *out_authorized                              = result == WEB_CONSOLE_HTTP_AUTH_OK;
    return *out_authorized ? ESP_OK : web_console_http_send_auth_error(request, result);
}

/** @brief 有界读取一个 query 参数。 */
static esp_err_t web_console_provider_get_query_value(httpd_req_t *request,
                                                      const char *key,
                                                      char *out_value,
                                                      size_t out_value_size)
{
    if (request == NULL || key == NULL || out_value == NULL || out_value_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t query_size = httpd_req_get_url_query_len(request);
    if (query_size == 0U)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (query_size > WEB_CONSOLE_PROVIDER_QUERY_MAX_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    char query[WEB_CONSOLE_PROVIDER_QUERY_MAX_BYTES + 1U]{};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = httpd_query_key_value(query, key, out_value, out_value_size);
    web_console_provider_secure_clear(query, sizeof(query));
    return error;
}

/** @brief 严格解析十进制 uint64 字符串。 */
static bool web_console_parse_uint64(const char *text, uint64_t *out_value)
{
    if (text == NULL || out_value == NULL)
    {
        return false;
    }
    const size_t length = strnlen(text, WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE);
    if (length == 0U || length >= WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE)
    {
        return false;
    }

    uint64_t value = 0U;
    for (size_t index = 0U; index < length; ++index)
    {
        const unsigned char digit = (unsigned char) text[index];
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        const uint64_t numeric_digit = (uint64_t) (digit - '0');
        if (value > (UINT64_MAX - numeric_digit) / 10U)
        {
            return false;
        }
        value = value * 10U + numeric_digit;
    }
    *out_value = value;
    return true;
}

/** @brief 把 uint64 编码为不会损失精度的十进制 JSON 字符串。 */
static bool web_console_format_uint64(uint64_t value,
                                      char out_text[WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE])
{
    const int length = snprintf(out_text, WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE, "%" PRIu64, value);
    return length > 0 && (size_t) length < WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE;
}

/** @brief 使用固定上限缓冲区编码并发送一个 cJSON 正文。 */
static esp_err_t web_console_provider_send_cjson(httpd_req_t *request,
                                                 const char *status,
                                                 const cJSON *root)
{
    char *body = static_cast<char *>(malloc(WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES));
    if (body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = ESP_ERR_INVALID_SIZE;
    if (cJSON_PrintPreallocated(
            const_cast<cJSON *>(root), body, WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES, false))
    {
        const size_t body_size = strnlen(body, WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES);
        if (body_size < WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES)
        {
            error = web_console_http_send_json(request, status, body, body_size);
        }
    }

    web_console_provider_secure_clear(body, WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES);
    free(body);
    return error;
}

/** @brief 使用固定上限缓冲区编码并发送一个 cJSON 分块。 */
static esp_err_t web_console_provider_send_cjson_chunk(httpd_req_t *request, const cJSON *item)
{
    char *body = static_cast<char *>(malloc(WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES));
    if (body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = ESP_ERR_INVALID_SIZE;
    if (cJSON_PrintPreallocated(
            const_cast<cJSON *>(item), body, WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES, false))
    {
        const size_t body_size = strnlen(body, WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES);
        if (body_size < WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES)
        {
            error = httpd_resp_send_chunk(request, body, (ssize_t) body_size);
        }
    }

    web_console_provider_secure_clear(body, WEB_CONSOLE_PROVIDER_JSON_ITEM_MAX_BYTES);
    free(body);
    return error;
}

/** @brief JSON 编码并分块发送一个字符串。 */
static esp_err_t web_console_provider_send_json_string_chunk(httpd_req_t *request, const char *text)
{
    cJSON *item = cJSON_CreateString(text);
    if (item == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t error = web_console_provider_send_cjson_chunk(request, item);
    cJSON_Delete(item);
    return error;
}

/** @brief 分块发送固定 JSON 字面量。 */
static esp_err_t web_console_provider_send_literal_chunk(httpd_req_t *request, const char *text)
{
    return httpd_resp_send_chunk(request, text, HTTPD_RESP_USE_STRLEN);
}

/** @brief 创建一个字段元数据 JSON 对象。 */
static cJSON *web_console_provider_create_field_info_json(const web_console_field_info_t *field)
{
    const char *type_name   = web_console_field_type_name(field->type);
    const char *effect_name = web_console_field_effect_name(field->effect);
    if (type_name == NULL || effect_name == NULL)
    {
        return NULL;
    }

    cJSON *object = cJSON_CreateObject();
    if (object == NULL || cJSON_AddStringToObject(object, "id", field->id) == NULL
        || cJSON_AddStringToObject(object, "label", field->label) == NULL
        || cJSON_AddStringToObject(object, "type", type_name) == NULL
        || cJSON_AddStringToObject(object, "effect", effect_name) == NULL)
    {
        cJSON_Delete(object);
        return NULL;
    }
    const char *optional_names[] = { "description", "unit", "summary", "format" };
    const char *optional_values[] = {
        field->description,
        field->unit,
        field->summary,
        field->format,
    };
    for (size_t index = 0U; index < sizeof(optional_names) / sizeof(optional_names[0]); ++index)
    {
        if (optional_values[index] != NULL
            && cJSON_AddStringToObject(object, optional_names[index], optional_values[index]) == NULL)
        {
            cJSON_Delete(object);
            return NULL;
        }
    }

    const bool secret = (field->access & WEB_CONSOLE_FIELD_ACCESS_SECRET) != 0U;
    const bool readable =
        !secret && (field->access & WEB_CONSOLE_FIELD_ACCESS_WRITE_ONLY) == 0U;
    const bool writable =
        !secret && (field->access & WEB_CONSOLE_FIELD_ACCESS_READ_ONLY) == 0U;
    if (cJSON_AddBoolToObject(object, "readable", readable) == NULL
        || cJSON_AddBoolToObject(object, "writable", writable) == NULL
        || cJSON_AddBoolToObject(object, "secret", secret) == NULL)
    {
        cJSON_Delete(object);
        return NULL;
    }

    switch (field->type)
    {
        case WEB_CONSOLE_FIELD_TYPE_INT32:
        case WEB_CONSOLE_FIELD_TYPE_UINT32:
            if (cJSON_AddNumberToObject(object, "min", (double) field->minimum) == NULL
                || cJSON_AddNumberToObject(object, "max", (double) field->maximum) == NULL
                || cJSON_AddNumberToObject(object, "step", (double) field->step) == NULL)
            {
                cJSON_Delete(object);
                return NULL;
            }
            break;

        case WEB_CONSOLE_FIELD_TYPE_STRING:
            if (cJSON_AddNumberToObject(object, "maxBytes", (double) field->max_length_bytes) == NULL)
            {
                cJSON_Delete(object);
                return NULL;
            }
            if (field->file_suffix != NULL
                && cJSON_AddStringToObject(object, "fileSuffix", field->file_suffix) == NULL)
            {
                cJSON_Delete(object);
                return NULL;
            }
            break;

        case WEB_CONSOLE_FIELD_TYPE_ENUM:
        {
            cJSON *enum_values = cJSON_AddArrayToObject(object, "options");
            if (enum_values == NULL)
            {
                cJSON_Delete(object);
                return NULL;
            }
            for (size_t index = 0U; index < field->enum_value_count; ++index)
            {
                cJSON *enum_value = cJSON_CreateObject();
                if (enum_value == NULL
                    || cJSON_AddNumberToObject(
                           enum_value, "value", (double) field->enum_values[index].value)
                           == NULL
                    || cJSON_AddStringToObject(
                           enum_value, "label", field->enum_values[index].label)
                           == NULL
                    || !cJSON_AddItemToArray(enum_values, enum_value))
                {
                    cJSON_Delete(enum_value);
                    cJSON_Delete(object);
                    return NULL;
                }
            }
            break;
        }

        case WEB_CONSOLE_FIELD_TYPE_BOOL:
        default:
            break;
    }
    return object;
}

/** @brief 分块发送一个 Provider 分区的完整元数据。 */
template <typename Provider>
static esp_err_t web_console_provider_send_section_info(httpd_req_t *request,
                                                        const Provider *provider)
{
    esp_err_t error = web_console_provider_send_literal_chunk(request, "{\"id\":");
    if (error == ESP_OK)
    {
        error = web_console_provider_send_json_string_chunk(request, provider->section_id);
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"label\":");
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_json_string_chunk(request, provider->label);
    }
    if (error == ESP_OK && provider->description != NULL)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"description\":");
        if (error == ESP_OK)
        {
            error = web_console_provider_send_json_string_chunk(request, provider->description);
        }
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"fields\":[");
    }

    for (size_t index = 0U; error == ESP_OK && index < provider->field_count; ++index)
    {
        if (index != 0U)
        {
            error = web_console_provider_send_literal_chunk(request, ",");
        }
        cJSON *field_json = error == ESP_OK
                                ? web_console_provider_create_field_info_json(&provider->fields[index])
                                : NULL;
        if (error == ESP_OK && field_json == NULL)
        {
            error = ESP_ERR_NO_MEM;
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_cjson_chunk(request, field_json);
        }
        cJSON_Delete(field_json);
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, "]}");
    }
    return error;
}

/** @brief 分块发送一个实际装配的 Settings 或 Status 模块。 */
template <typename Provider>
static esp_err_t web_console_provider_send_module(httpd_req_t *request,
                                                  const char *module_id,
                                                  const char *module_label,
                                                  size_t provider_count,
                                                  const Provider *(*get_provider)(size_t))
{
    esp_err_t error = web_console_provider_send_literal_chunk(request, "{\"id\":");
    if (error == ESP_OK)
    {
        error = web_console_provider_send_json_string_chunk(request, module_id);
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"label\":");
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_json_string_chunk(request, module_label);
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"sections\":[");
    }
    for (size_t index = 0U; error == ESP_OK && index < provider_count; ++index)
    {
        if (index != 0U)
        {
            error = web_console_provider_send_literal_chunk(request, ",");
        }
        const Provider *provider = error == ESP_OK ? get_provider(index) : NULL;
        if (provider == NULL)
        {
            error = ESP_ERR_INVALID_STATE;
        }
        else
        {
            error = web_console_provider_send_section_info(request, provider);
        }
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, "]}");
    }
    return error;
}

#if CONFIG_WEB_CONSOLE_ACTIONS
/** @brief 分块发送一个管理操作的完整元数据。 */
static esp_err_t web_console_provider_send_action_info(
    httpd_req_t *request,
    const web_console_action_info_t *action)
{
    esp_err_t error = web_console_provider_send_literal_chunk(request, "{\"id\":");
    if (error == ESP_OK)
    {
        error = web_console_provider_send_json_string_chunk(request, action->id);
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"label\":");
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_json_string_chunk(request, action->label);
    }
    if (error == ESP_OK && action->description != NULL)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"description\":");
        if (error == ESP_OK)
        {
            error = web_console_provider_send_json_string_chunk(request, action->description);
        }
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"inputs\":[");
    }
    for (size_t index = 0U; error == ESP_OK && index < action->input_field_count; ++index)
    {
        if (index != 0U)
        {
            error = web_console_provider_send_literal_chunk(request, ",");
        }
        cJSON *field_json = error == ESP_OK
                                ? web_console_provider_create_field_info_json(
                                      &action->input_fields[index])
                                : NULL;
        if (error == ESP_OK && field_json == NULL)
        {
            error = ESP_ERR_NO_MEM;
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_cjson_chunk(request, field_json);
        }
        cJSON_Delete(field_json);
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, "]}");
    }
    return error;
}

/** @brief 分块发送实际装配的 Actions 模块。 */
static esp_err_t web_console_provider_send_actions_module(httpd_req_t *request,
                                                          size_t provider_count)
{
    esp_err_t error = web_console_provider_send_literal_chunk(
        request, "{\"id\":\"actions\",\"label\":\"操作\",\"sections\":[");
    for (size_t index = 0U; error == ESP_OK && index < provider_count; ++index)
    {
        if (index != 0U)
        {
            error = web_console_provider_send_literal_chunk(request, ",");
        }
        const web_console_action_provider_t *provider =
            web_console_provider_registry_get_action(index);
        if (provider == NULL)
        {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        error = web_console_provider_send_literal_chunk(request, "{\"id\":");
        if (error == ESP_OK)
        {
            error = web_console_provider_send_json_string_chunk(request, provider->section_id);
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_literal_chunk(request, ",\"label\":");
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_json_string_chunk(request, provider->label);
        }
        if (error == ESP_OK && provider->description != NULL)
        {
            error = web_console_provider_send_literal_chunk(request, ",\"description\":");
            if (error == ESP_OK)
            {
                error = web_console_provider_send_json_string_chunk(request, provider->description);
            }
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_literal_chunk(request, ",\"actions\":[");
        }
        for (size_t action_index = 0U;
             error == ESP_OK && action_index < provider->action_count;
             ++action_index)
        {
            if (action_index != 0U)
            {
                error = web_console_provider_send_literal_chunk(request, ",");
            }
            if (error == ESP_OK)
            {
                error = web_console_provider_send_action_info(
                    request, &provider->actions[action_index]);
            }
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_literal_chunk(request, "]}");
        }
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, "]}");
    }
    return error;
}
#endif

esp_err_t web_console_provider_handle_capabilities_get(httpd_req_t *request)
{
    bool      authorized = false;
    esp_err_t error      = web_console_provider_authorize(request, &authorized);
    if (!authorized)
    {
        return error;
    }

    const size_t settings_count = web_console_provider_registry_get_settings_count();
    const size_t status_count   = web_console_provider_registry_get_status_count();
#if CONFIG_WEB_CONSOLE_ACTIONS
    const size_t action_count   = web_console_provider_registry_get_action_count();
#endif

    error = web_console_http_set_json_response(request, "200 OK");
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, "{\"schema\":1,\"modules\":[");
    }

    bool module_sent = false;
#if CONFIG_WEB_CONSOLE_FILES
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(
            request, "{\"id\":\"files\",\"label\":\"文件\"}");
        module_sent = error == ESP_OK;
    }
#endif
#if CONFIG_WEB_CONSOLE_SETTINGS
    if (error == ESP_OK && settings_count > 0U)
    {
        if (module_sent)
        {
            error = web_console_provider_send_literal_chunk(request, ",");
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_module(
                request,
                "settings",
                "设置",
                settings_count,
                web_console_provider_registry_get_settings);
        }
        module_sent = error == ESP_OK;
    }
#else
    (void) settings_count;
#endif
#if CONFIG_WEB_CONSOLE_STATUS
    if (error == ESP_OK && status_count > 0U)
    {
        if (module_sent)
        {
            error = web_console_provider_send_literal_chunk(request, ",");
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_module(
                request,
                "status",
                "状态",
                status_count,
                web_console_provider_registry_get_status);
        }
        module_sent = error == ESP_OK;
    }
#else
    (void) status_count;
#endif
#if CONFIG_WEB_CONSOLE_ACTIONS
    if (error == ESP_OK && action_count > 0U)
    {
        if (module_sent)
        {
            error = web_console_provider_send_literal_chunk(request, ",");
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_actions_module(request, action_count);
        }
    }
#endif

    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, "]}");
    }
    if (error == ESP_OK)
    {
        error = httpd_resp_send_chunk(request, NULL, 0U);
    }
    return error;
}

/** @brief 检查整数值是否满足描述符范围和步长。 */
static bool web_console_integer_value_is_valid(const web_console_field_info_t *field,
                                               int64_t value)
{
    if (value < field->minimum || value > field->maximum)
    {
        return false;
    }
    const uint64_t offset = (uint64_t) (value - field->minimum);
    return offset % field->step == 0U;
}

/** @brief 检查枚举协议值是否存在于描述符表。 */
static bool web_console_enum_value_is_valid(const web_console_field_info_t *field,
                                            int32_t value)
{
    for (size_t index = 0U; index < field->enum_value_count; ++index)
    {
        if (field->enum_values[index].value == value)
        {
            return true;
        }
    }
    return false;
}

/** @brief 校验一个普通字段值的类型及元数据约束。 */
static bool web_console_field_value_is_valid(const web_console_field_info_t *field,
                                             const web_console_field_value_t *value)
{
    if (field == NULL || value == NULL || value->type != field->type || !value->configured)
    {
        return false;
    }

    switch (field->type)
    {
        case WEB_CONSOLE_FIELD_TYPE_BOOL:
            return true;
        case WEB_CONSOLE_FIELD_TYPE_INT32:
            return web_console_integer_value_is_valid(field, value->data.int32_value);
        case WEB_CONSOLE_FIELD_TYPE_UINT32:
            return web_console_integer_value_is_valid(field, value->data.uint32_value);
        case WEB_CONSOLE_FIELD_TYPE_STRING:
        {
            const size_t length = strnlen(
                value->data.string_value, WEB_CONSOLE_PROVIDER_STRING_MAX_LENGTH + 1U);
            return length <= field->max_length_bytes
                   && length <= WEB_CONSOLE_PROVIDER_STRING_MAX_LENGTH
                   && web_console_provider_utf8_is_valid(value->data.string_value, length);
        }
        case WEB_CONSOLE_FIELD_TYPE_ENUM:
            return web_console_enum_value_is_valid(field, value->data.int32_value);
        default:
            return false;
    }
}

/** @brief 检查 Secret/Write-only 快照没有携带被隐藏的 union 数据。 */
static bool web_console_redacted_value_is_valid(const web_console_field_info_t *field,
                                                const web_console_field_value_t *value)
{
    uint8_t zero[sizeof(value->data)]{};
    return value->type == field->type
           && memcmp(&value->data, zero, sizeof(value->data)) == 0;
}

/** @brief 校验 Provider 完整快照的缓冲区和逐字段输出契约。 */
static bool web_console_settings_snapshot_is_valid(
    const web_console_settings_provider_t *provider,
    const web_console_settings_snapshot_t *snapshot,
    const web_console_field_value_t *expected_values)
{
    if (snapshot->values != expected_values || snapshot->value_capacity != provider->field_count
        || snapshot->value_count != provider->field_count)
    {
        return false;
    }
    for (size_t index = 0U; index < provider->field_count; ++index)
    {
        const web_console_field_info_t *field = &provider->fields[index];
        const web_console_field_value_t *value = &snapshot->values[index];
        const bool redacted = (field->access
                               & (WEB_CONSOLE_FIELD_ACCESS_SECRET
                                  | WEB_CONSOLE_FIELD_ACCESS_WRITE_ONLY))
                              != 0U;
        if ((redacted && !web_console_redacted_value_is_valid(field, value))
            || (!redacted && !web_console_field_value_is_valid(field, value)))
        {
            return false;
        }
    }
    return true;
}

#if CONFIG_WEB_CONSOLE_STATUS
/** @brief 校验 Status Provider 完整输出契约。 */
static bool web_console_status_is_valid(const web_console_status_provider_t *provider,
                                        const web_console_section_status_t *status,
                                        const web_console_field_value_t *expected_values)
{
    if (status->values != expected_values || status->value_capacity != provider->field_count
        || status->value_count != provider->field_count)
    {
        return false;
    }
    for (size_t index = 0U; index < provider->field_count; ++index)
    {
        if (!web_console_field_value_is_valid(&provider->fields[index], &status->values[index]))
        {
            return false;
        }
    }
    return true;
}
#endif

/** @brief 创建一个字段公开值 JSON 对象；Secret/Write-only 不编码原值。 */
static cJSON *web_console_provider_create_field_value_json(
    const web_console_field_info_t *field,
    const web_console_field_value_t *value)
{
    cJSON *object = cJSON_CreateObject();
    if (object == NULL || cJSON_AddStringToObject(object, "id", field->id) == NULL
        || cJSON_AddBoolToObject(object, "configured", value->configured) == NULL)
    {
        cJSON_Delete(object);
        return NULL;
    }

    if ((field->access
         & (WEB_CONSOLE_FIELD_ACCESS_SECRET | WEB_CONSOLE_FIELD_ACCESS_WRITE_ONLY))
        != 0U)
    {
        return object;
    }

    cJSON *encoded_value = NULL;
    switch (field->type)
    {
        case WEB_CONSOLE_FIELD_TYPE_BOOL:
            encoded_value = cJSON_CreateBool(value->data.boolean_value);
            break;
        case WEB_CONSOLE_FIELD_TYPE_INT32:
        case WEB_CONSOLE_FIELD_TYPE_ENUM:
            encoded_value = cJSON_CreateNumber((double) value->data.int32_value);
            break;
        case WEB_CONSOLE_FIELD_TYPE_UINT32:
            encoded_value = cJSON_CreateNumber((double) value->data.uint32_value);
            break;
        case WEB_CONSOLE_FIELD_TYPE_STRING:
            encoded_value = cJSON_CreateString(value->data.string_value);
            break;
        default:
            break;
    }
    if (encoded_value == NULL || !cJSON_AddItemToObject(object, "value", encoded_value))
    {
        cJSON_Delete(encoded_value);
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

/** @brief 分块发送一个 Settings 或 Status 分区的字段值。 */
static esp_err_t web_console_provider_send_section_values(
    httpd_req_t *request,
    const char *section_id,
    uint64_t version,
    const web_console_field_info_t *fields,
    const web_console_field_value_t *values,
    size_t field_count)
{
    char version_text[WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE]{};
    if (!web_console_format_uint64(version, version_text))
    {
        return ESP_FAIL;
    }

    esp_err_t error = web_console_http_set_json_response(request, "200 OK");
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, "{\"section\":");
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_json_string_chunk(request, section_id);
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"version\":");
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_json_string_chunk(request, version_text);
    }
    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, ",\"values\":[");
    }

    for (size_t index = 0U; error == ESP_OK && index < field_count; ++index)
    {
        if (index != 0U)
        {
            error = web_console_provider_send_literal_chunk(request, ",");
        }
        cJSON *value_json = error == ESP_OK
                                ? web_console_provider_create_field_value_json(
                                      &fields[index], &values[index])
                                : NULL;
        if (error == ESP_OK && value_json == NULL)
        {
            error = ESP_ERR_NO_MEM;
        }
        if (error == ESP_OK)
        {
            error = web_console_provider_send_cjson_chunk(request, value_json);
        }
        cJSON_Delete(value_json);
    }

    if (error == ESP_OK)
    {
        error = web_console_provider_send_literal_chunk(request, "]}");
    }
    if (error == ESP_OK)
    {
        error = httpd_resp_send_chunk(request, NULL, 0U);
    }
    web_console_provider_secure_clear(version_text, sizeof(version_text));
    return error;
}

/** @brief 读取并查找请求指定的 Settings 分区。 */
static const web_console_settings_provider_t *web_console_provider_find_settings_request(
    httpd_req_t *request,
    char out_section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U],
    esp_err_t *out_error)
{
    const esp_err_t query_error = web_console_provider_get_query_value(
        request, "section", out_section, WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U);
    if (query_error != ESP_OK)
    {
        *out_error = web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"缺少或无效的 section 参数\"}");
        return NULL;
    }
    const web_console_settings_provider_t *provider =
        web_console_provider_registry_find_settings(out_section);
    if (provider == NULL)
    {
        *out_error = web_console_provider_send_error(
            request,
            "404 Not Found",
            "{\"error\":\"section_not_found\",\"message\":\"设置分区不存在\"}");
    }
    return provider;
}

#if CONFIG_WEB_CONSOLE_STATUS
/** @brief 读取并查找请求指定的 Status 分区。 */
static const web_console_status_provider_t *web_console_provider_find_status_request(
    httpd_req_t *request,
    char out_section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U],
    esp_err_t *out_error)
{
    const esp_err_t query_error = web_console_provider_get_query_value(
        request, "section", out_section, WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U);
    if (query_error != ESP_OK)
    {
        *out_error = web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"缺少或无效的 section 参数\"}");
        return NULL;
    }
    const web_console_status_provider_t *provider =
        web_console_provider_registry_find_status(out_section);
    if (provider == NULL)
    {
        *out_error = web_console_provider_send_error(
            request,
            "404 Not Found",
            "{\"error\":\"section_not_found\",\"message\":\"状态分区不存在\"}");
    }
    return provider;
}
#endif

#if CONFIG_WEB_CONSOLE_SETTINGS || CONFIG_WEB_CONSOLE_ACTIONS
#if CONFIG_WEB_CONSOLE_SETTINGS
/** @brief 返回指定 Settings 分区的完整公开快照。 */
static esp_err_t web_console_provider_handle_settings_get(httpd_req_t *request)
{
    bool      authorized = false;
    esp_err_t error      = web_console_provider_authorize(request, &authorized);
    if (!authorized)
    {
        return error;
    }

    char section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U]{};
    const web_console_settings_provider_t *provider =
        web_console_provider_find_settings_request(request, section, &error);
    if (provider == NULL)
    {
        return error;
    }

    web_console_field_value_t *values = static_cast<web_console_field_value_t *>(
        calloc(provider->field_count, sizeof(web_console_field_value_t)));
    if (values == NULL)
    {
        return web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"internal_error\",\"message\":\"服务内部资源不足\"}");
    }

    web_console_settings_snapshot_t snapshot = {
        .version        = 0U,
        .values         = values,
        .value_capacity = provider->field_count,
        .value_count    = 0U,
    };
    const esp_err_t provider_error =
        provider->get_snapshot_copy(provider->context, &snapshot);
    if (provider_error != ESP_OK)
    {
        error = web_console_provider_send_error(
            request,
            "503 Service Unavailable",
            "{\"error\":\"provider_unavailable\",\"message\":\"设置暂时不可读取\"}");
    }
    else if (!web_console_settings_snapshot_is_valid(provider, &snapshot, values))
    {
        error = web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"provider_contract\",\"message\":\"设置提供者返回了无效结果\"}");
    }
    else
    {
        error = web_console_provider_send_section_values(
            request,
            provider->section_id,
            snapshot.version,
            provider->fields,
            values,
            provider->field_count);
    }

    web_console_provider_secure_clear(values,
                                      provider->field_count * sizeof(web_console_field_value_t));
    free(values);
    web_console_provider_secure_clear(section, sizeof(section));
    return error;
}
#endif

/** @brief 判断 Content-Type 是否为 JSON。 */
static bool web_console_provider_content_type_is_json(httpd_req_t *request)
{
    static const char prefix[] = "application/json";
    char value[sizeof(prefix) + 1U]{};
    const esp_err_t error =
        httpd_req_get_hdr_value_str(request, "Content-Type", value, sizeof(value));
    return (error == ESP_OK || error == ESP_ERR_HTTPD_RESULT_TRUNC)
           && strncasecmp(value, prefix, sizeof(prefix) - 1U) == 0
           && (value[sizeof(prefix) - 1U] == '\0'
               || value[sizeof(prefix) - 1U] == ';');
}

/** @brief 在 cJSON 解析前限制嵌套深度，避免小正文形成深递归。 */
static bool web_console_provider_json_depth_is_bounded(const char *body, size_t body_size)
{
    size_t depth     = 0U;
    bool   in_string = false;
    bool   escaped   = false;
    for (size_t index = 0U; index < body_size; ++index)
    {
        const char value = body[index];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (value == '\\')
            {
                if (index + 5U < body_size && body[index + 1U] == 'u'
                    && body[index + 2U] == '0' && body[index + 3U] == '0'
                    && body[index + 4U] == '0' && body[index + 5U] == '0')
                {
                    return false;
                }
                escaped = true;
            }
            else if (value == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (value == '"')
        {
            in_string = true;
        }
        else if (value == '{' || value == '[')
        {
            ++depth;
            if (depth > WEB_CONSOLE_PROVIDER_JSON_DEPTH_MAX)
            {
                return false;
            }
        }
        else if (value == '}' || value == ']')
        {
            if (depth == 0U)
            {
                return false;
            }
            --depth;
        }
    }
    return !in_string && depth == 0U;
}

/** @brief 有界接收并解析 PATCH JSON 正文。 */
static cJSON *web_console_provider_receive_json(httpd_req_t *request, bool *out_io_failed)
{
    *out_io_failed = false;
    const size_t body_size = request->content_len;
    char *body = static_cast<char *>(malloc(body_size + 1U));
    if (body == NULL)
    {
        return NULL;
    }

    size_t received = 0U;
    while (received < body_size)
    {
        const int chunk = httpd_req_recv(request, body + received, body_size - received);
        if (chunk <= 0)
        {
            *out_io_failed = true;
            web_console_provider_secure_clear(body, body_size + 1U);
            free(body);
            return NULL;
        }
        received += (size_t) chunk;
    }
    body[body_size] = '\0';

    cJSON *root = NULL;
    if (web_console_provider_json_depth_is_bounded(body, body_size))
    {
        root = cJSON_ParseWithLengthOpts(body, body_size + 1U, NULL, true);
    }
    web_console_provider_secure_clear(body, body_size + 1U);
    free(body);
    return root;
}

/** @brief 校验 JSON 对象恰好包含指定且不重复的属性。 */
static bool web_console_provider_object_has_keys(const cJSON *object,
                                                 const char *const *keys,
                                                 size_t key_count)
{
    if (!cJSON_IsObject(object) || key_count > 2U)
    {
        return false;
    }
    bool seen[2]{};
    size_t member_count = 0U;
    for (const cJSON *member = object->child; member != NULL; member = member->next)
    {
        ++member_count;
        bool matched = false;
        for (size_t index = 0U; index < key_count; ++index)
        {
            if (member->string != NULL && strcmp(member->string, keys[index]) == 0)
            {
                if (seen[index])
                {
                    return false;
                }
                seen[index] = true;
                matched     = true;
                break;
            }
        }
        if (!matched)
        {
            return false;
        }
    }
    if (member_count != key_count)
    {
        return false;
    }
    for (size_t index = 0U; index < key_count; ++index)
    {
        if (!seen[index])
        {
            return false;
        }
    }
    return true;
}

/** @brief 按稳定 ID 查找字段描述符索引。 */
static bool web_console_provider_find_field(const web_console_field_info_t *fields,
                                            size_t field_count,
                                            const char *field_id,
                                            size_t *out_index)
{
    for (size_t index = 0U; index < field_count; ++index)
    {
        if (strcmp(fields[index].id, field_id) == 0)
        {
            *out_index = index;
            return true;
        }
    }
    return false;
}

/** @brief 严格解析一个 JSON 整数。 */
static bool web_console_provider_read_json_integer(const cJSON *item, int64_t *out_value)
{
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)
        || std::trunc(item->valuedouble) != item->valuedouble
        || item->valuedouble < (double) INT32_MIN || item->valuedouble > (double) UINT32_MAX)
    {
        return false;
    }
    *out_value = (int64_t) item->valuedouble;
    return true;
}

/** @brief 按字段描述符解析并校验一个 PATCH 值。 */
static bool web_console_provider_parse_change_value(const web_console_field_info_t *field,
                                                    const cJSON *item,
                                                    web_console_field_value_t *out_value)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type       = field->type;
    out_value->configured = true;

    int64_t integer_value = 0;
    switch (field->type)
    {
        case WEB_CONSOLE_FIELD_TYPE_BOOL:
            if (!cJSON_IsBool(item))
            {
                return false;
            }
            out_value->data.boolean_value = cJSON_IsTrue(item);
            return true;

        case WEB_CONSOLE_FIELD_TYPE_INT32:
            if (!web_console_provider_read_json_integer(item, &integer_value)
                || integer_value < INT32_MIN || integer_value > INT32_MAX
                || !web_console_integer_value_is_valid(field, integer_value))
            {
                return false;
            }
            out_value->data.int32_value = (int32_t) integer_value;
            return true;

        case WEB_CONSOLE_FIELD_TYPE_UINT32:
            if (!web_console_provider_read_json_integer(item, &integer_value)
                || integer_value < 0 || integer_value > UINT32_MAX
                || !web_console_integer_value_is_valid(field, integer_value))
            {
                return false;
            }
            out_value->data.uint32_value = (uint32_t) integer_value;
            return true;

        case WEB_CONSOLE_FIELD_TYPE_STRING:
        {
            if (!cJSON_IsString(item) || item->valuestring == NULL)
            {
                return false;
            }
            const size_t length = strnlen(
                item->valuestring, WEB_CONSOLE_PROVIDER_STRING_MAX_LENGTH + 1U);
            if (length > field->max_length_bytes
                || length > WEB_CONSOLE_PROVIDER_STRING_MAX_LENGTH
                || !web_console_provider_utf8_is_valid(item->valuestring, length))
            {
                return false;
            }
            memcpy(out_value->data.string_value, item->valuestring, length + 1U);
            return true;
        }

        case WEB_CONSOLE_FIELD_TYPE_ENUM:
            if (!web_console_provider_read_json_integer(item, &integer_value)
                || integer_value < INT32_MIN || integer_value > INT32_MAX
                || !web_console_enum_value_is_valid(field, (int32_t) integer_value))
            {
                return false;
            }
            out_value->data.int32_value = (int32_t) integer_value;
            return true;

        default:
            return false;
    }
}

#if CONFIG_WEB_CONSOLE_SETTINGS
/** @brief 解析完整 Settings update，不保存任何 cJSON 指针。 */
static web_console_update_parse_result_t web_console_provider_parse_update(
    const cJSON *root,
    const web_console_settings_provider_t *provider,
    web_console_settings_update_field_t *out_fields,
    web_console_settings_update_t *out_update)
{
    static const char *const root_keys[]   = { "expectedVersion", "changes" };
    static const char *const change_keys[] = { "id", "value" };
    if (!web_console_provider_object_has_keys(root, root_keys, 2U))
    {
        return WEB_CONSOLE_UPDATE_PARSE_MALFORMED;
    }

    const cJSON *expected_version =
        cJSON_GetObjectItemCaseSensitive(root, "expectedVersion");
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(root, "changes");
    if (!cJSON_IsString(expected_version) || expected_version->valuestring == NULL
        || !web_console_parse_uint64(expected_version->valuestring, &out_update->expected_version)
        || !cJSON_IsArray(changes))
    {
        return WEB_CONSOLE_UPDATE_PARSE_MALFORMED;
    }

    const int change_count = cJSON_GetArraySize(changes);
    if (change_count <= 0 || (size_t) change_count > provider->field_count
        || (size_t) change_count > WEB_CONSOLE_PROVIDER_MAX_FIELDS_PER_SECTION)
    {
        return WEB_CONSOLE_UPDATE_PARSE_INVALID_CHANGE;
    }

    bool seen_fields[WEB_CONSOLE_PROVIDER_MAX_FIELDS_PER_SECTION]{};
    for (int change_index = 0; change_index < change_count; ++change_index)
    {
        const cJSON *change = cJSON_GetArrayItem(changes, change_index);
        if (!web_console_provider_object_has_keys(change, change_keys, 2U))
        {
            return WEB_CONSOLE_UPDATE_PARSE_INVALID_CHANGE;
        }
        const cJSON *id    = cJSON_GetObjectItemCaseSensitive(change, "id");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(change, "value");
        if (!cJSON_IsString(id) || id->valuestring == NULL)
        {
            return WEB_CONSOLE_UPDATE_PARSE_INVALID_CHANGE;
        }

        size_t field_index = 0U;
        if (!web_console_provider_find_field(provider->fields,
                                             provider->field_count,
                                             id->valuestring,
                                             &field_index)
            || seen_fields[field_index])
        {
            return WEB_CONSOLE_UPDATE_PARSE_INVALID_CHANGE;
        }
        seen_fields[field_index] = true;

        const web_console_field_info_t *field = &provider->fields[field_index];
        if ((field->access & WEB_CONSOLE_FIELD_ACCESS_SECRET) != 0U)
        {
            return WEB_CONSOLE_UPDATE_PARSE_SECRET;
        }
        if ((field->access & WEB_CONSOLE_FIELD_ACCESS_READ_ONLY) != 0U
            || !web_console_provider_parse_change_value(
                field, value, &out_fields[change_index].value))
        {
            return WEB_CONSOLE_UPDATE_PARSE_INVALID_CHANGE;
        }
        out_fields[change_index].field_index = field_index;
    }

    out_update->fields      = out_fields;
    out_update->field_count = (size_t) change_count;
    return WEB_CONSOLE_UPDATE_PARSE_OK;
}

/** @brief 把 Provider 对 update 的同步拒绝映射为固定 HTTP 错误。 */
static esp_err_t web_console_provider_send_update_rejection(httpd_req_t *request,
                                                            esp_err_t provider_error)
{
    if (provider_error == ESP_ERR_INVALID_VERSION)
    {
        return web_console_provider_send_error(
            request,
            "409 Conflict",
            "{\"error\":\"version_conflict\",\"message\":\"设置版本已变化，请刷新后重试\"}");
    }
    if (provider_error == ESP_ERR_INVALID_ARG)
    {
        return web_console_provider_send_error(
            request,
            "422 Unprocessable Content",
            "{\"error\":\"invalid_change\",\"message\":\"设置组合不满足约束\"}");
    }
    if (provider_error == ESP_ERR_INVALID_STATE)
    {
        return web_console_provider_send_error(
            request,
            "409 Conflict",
            "{\"error\":\"update_busy\",\"message\":\"当前状态暂不接受设置更新\"}");
    }
    return web_console_provider_send_error(
        request,
        "503 Service Unavailable",
        "{\"error\":\"provider_unavailable\",\"message\":\"设置更新暂时不可用\"}");
}

/** @brief 接收、校验并异步提交一个 Settings update。 */
static esp_err_t web_console_provider_handle_settings_patch(httpd_req_t *request)
{
    bool      authorized = false;
    esp_err_t error      = web_console_provider_authorize(request, &authorized);
    if (!authorized)
    {
        return error;
    }

    char section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U]{};
    const web_console_settings_provider_t *provider =
        web_console_provider_find_settings_request(request, section, &error);
    if (provider == NULL)
    {
        return error;
    }

    if (!web_console_provider_content_type_is_json(request))
    {
        return web_console_provider_send_error(
            request,
            "415 Unsupported Media Type",
            "{\"error\":\"unsupported_media_type\",\"message\":\"请求正文必须是 JSON\"}");
    }
    if (request->content_len == 0U)
    {
        return web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"请求正文不能为空\"}");
    }
    if (request->content_len > WEB_CONSOLE_PROVIDER_REQUEST_BODY_MAX_BYTES)
    {
        return web_console_provider_send_error(
            request,
            "413 Content Too Large",
            "{\"error\":\"payload_too_large\",\"message\":\"设置请求正文过大\"}");
    }

    bool   io_failed = false;
    cJSON *root      = web_console_provider_receive_json(request, &io_failed);
    if (root == NULL)
    {
        error = web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"设置请求 JSON 无效\"}");
        return io_failed ? ESP_FAIL : error;
    }

    web_console_settings_update_field_t *fields =
        static_cast<web_console_settings_update_field_t *>(
            calloc(provider->field_count, sizeof(web_console_settings_update_field_t)));
    if (fields == NULL)
    {
        cJSON_Delete(root);
        return web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"internal_error\",\"message\":\"服务内部资源不足\"}");
    }

    web_console_settings_update_t update{};
    const web_console_update_parse_result_t parse_result =
        web_console_provider_parse_update(root, provider, fields, &update);
    cJSON_Delete(root);

    if (parse_result != WEB_CONSOLE_UPDATE_PARSE_OK)
    {
        web_console_provider_secure_clear(
            fields, provider->field_count * sizeof(web_console_settings_update_field_t));
        free(fields);
        if (parse_result == WEB_CONSOLE_UPDATE_PARSE_SECRET)
        {
            return web_console_provider_send_error(
                request,
                "403 Forbidden",
                "{\"error\":\"secret_write_forbidden\",\"message\":\"当前连接不允许修改秘密字段\"}");
        }
        return web_console_provider_send_error(
            request,
            parse_result == WEB_CONSOLE_UPDATE_PARSE_MALFORMED
                ? "400 Bad Request"
                : "422 Unprocessable Content",
            parse_result == WEB_CONSOLE_UPDATE_PARSE_MALFORMED
                ? "{\"error\":\"bad_request\",\"message\":\"设置请求结构无效\"}"
                : "{\"error\":\"invalid_change\",\"message\":\"设置字段或值不满足约束\"}");
    }

    const esp_err_t validation_error =
        provider->validate_update(provider->context, &update);
    if (validation_error != ESP_OK)
    {
        web_console_provider_secure_clear(
            fields, provider->field_count * sizeof(web_console_settings_update_field_t));
        free(fields);
        return web_console_provider_send_update_rejection(request, validation_error);
    }

    uint64_t request_id = 0U;
    const esp_err_t request_error =
        provider->request_update_copy(provider->context, &update, &request_id);
    web_console_provider_secure_clear(
        fields, provider->field_count * sizeof(web_console_settings_update_field_t));
    free(fields);

    if (request_error != ESP_OK)
    {
        return web_console_provider_send_update_rejection(request, request_error);
    }
    if (request_id == 0U)
    {
        return web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"provider_contract\",\"message\":\"设置提供者返回了无效请求标识\"}");
    }

    char request_text[WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE]{};
    cJSON *response = cJSON_CreateObject();
    if (!web_console_format_uint64(request_id, request_text) || response == NULL
        || cJSON_AddStringToObject(response, "section", provider->section_id) == NULL
        || cJSON_AddStringToObject(response, "state", "pending") == NULL
        || cJSON_AddStringToObject(response, "requestId", request_text) == NULL)
    {
        cJSON_Delete(response);
        web_console_provider_secure_clear(request_text, sizeof(request_text));
        return ESP_ERR_NO_MEM;
    }
    error = web_console_provider_send_cjson(request, "202 Accepted", response);
    cJSON_Delete(response);
    web_console_provider_secure_clear(request_text, sizeof(request_text));
    web_console_provider_secure_clear(section, sizeof(section));
    return error;
}

/** @brief 校验一次 Provider 异步更新结果。 */
static bool web_console_update_result_is_valid(const web_console_settings_update_result_t *result)
{
    switch (result->state)
    {
        case WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING:
        case WEB_CONSOLE_SETTINGS_UPDATE_STATE_SUCCEEDED:
            return result->error == ESP_OK;
        case WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED:
            return result->error != ESP_OK;
        default:
            return false;
    }
}

/** @brief 返回异步 Settings update 当前或最终结果。 */
static esp_err_t web_console_provider_handle_settings_result_get(httpd_req_t *request)
{
    bool      authorized = false;
    esp_err_t error      = web_console_provider_authorize(request, &authorized);
    if (!authorized)
    {
        return error;
    }

    char section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U]{};
    const web_console_settings_provider_t *provider =
        web_console_provider_find_settings_request(request, section, &error);
    if (provider == NULL)
    {
        return error;
    }

    char request_text[WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE]{};
    uint64_t request_id = 0U;
    if (web_console_provider_get_query_value(
            request, "request", request_text, sizeof(request_text))
            != ESP_OK
        || !web_console_parse_uint64(request_text, &request_id) || request_id == 0U)
    {
        web_console_provider_secure_clear(request_text, sizeof(request_text));
        return web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"缺少或无效的 request 参数\"}");
    }

    web_console_settings_update_result_t result{};
    const esp_err_t provider_error =
        provider->get_update_result_copy(provider->context, request_id, &result);
    if (provider_error == ESP_ERR_NOT_FOUND)
    {
        error = web_console_provider_send_error(
            request,
            "404 Not Found",
            "{\"error\":\"request_not_found\",\"message\":\"设置更新请求不存在或已过期\"}");
    }
    else if (provider_error != ESP_OK)
    {
        error = web_console_provider_send_error(
            request,
            "503 Service Unavailable",
            "{\"error\":\"provider_unavailable\",\"message\":\"设置更新结果暂时不可读取\"}");
    }
    else if (!web_console_update_result_is_valid(&result))
    {
        error = web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"provider_contract\",\"message\":\"设置提供者返回了无效结果\"}");
    }
    else
    {
        char version_text[WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE]{};
        cJSON *response = cJSON_CreateObject();
        const char *state = result.state == WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING
                                ? "pending"
                            : result.state == WEB_CONSOLE_SETTINGS_UPDATE_STATE_SUCCEEDED
                                ? "succeeded"
                                : "failed";
        if (!web_console_format_uint64(result.version, version_text) || response == NULL
            || cJSON_AddStringToObject(response, "section", provider->section_id) == NULL
            || cJSON_AddStringToObject(response, "request", request_text) == NULL
            || cJSON_AddStringToObject(response, "state", state) == NULL
            || cJSON_AddStringToObject(response, "version", version_text) == NULL
            || (result.state == WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED
                && cJSON_AddStringToObject(response, "error", "update_failed") == NULL))
        {
            error = ESP_ERR_NO_MEM;
        }
        else
        {
            error = web_console_provider_send_cjson(
                request,
                result.state == WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING
                    ? "202 Accepted"
                    : "200 OK",
                response);
        }
        cJSON_Delete(response);
        web_console_provider_secure_clear(version_text, sizeof(version_text));
    }

    web_console_provider_secure_clear(request_text, sizeof(request_text));
    web_console_provider_secure_clear(section, sizeof(section));
    return error;
}
#endif

#if CONFIG_WEB_CONSOLE_ACTIONS
/** @brief 读取并查找请求指定的 Actions 分区与操作。 */
static const web_console_action_provider_t *web_console_provider_find_action_request(
    httpd_req_t *request,
    char out_section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U],
    char out_action[WEB_CONSOLE_PROVIDER_FIELD_ID_MAX_LENGTH + 1U],
    size_t *out_action_index,
    esp_err_t *out_error)
{
    if (web_console_provider_get_query_value(
            request, "section", out_section, WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U)
            != ESP_OK
        || web_console_provider_get_query_value(
            request, "action", out_action, WEB_CONSOLE_PROVIDER_FIELD_ID_MAX_LENGTH + 1U)
            != ESP_OK)
    {
        *out_error = web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"缺少或无效的 section/action 参数\"}");
        return NULL;
    }

    const web_console_action_provider_t *provider =
        web_console_provider_registry_find_action(out_section);
    if (provider == NULL)
    {
        *out_error = web_console_provider_send_error(
            request,
            "404 Not Found",
            "{\"error\":\"section_not_found\",\"message\":\"操作分区不存在\"}");
        return NULL;
    }
    for (size_t index = 0U; index < provider->action_count; ++index)
    {
        if (strcmp(provider->actions[index].id, out_action) == 0)
        {
            *out_action_index = index;
            return provider;
        }
    }
    *out_error = web_console_provider_send_error(
        request,
        "404 Not Found",
        "{\"error\":\"action_not_found\",\"message\":\"管理操作不存在\"}");
    return NULL;
}

/** @brief 解析一次 Actions 输入，不保留任何 cJSON 指针。 */
static web_console_action_parse_result_t web_console_provider_parse_action_request(
    const cJSON *root,
    const web_console_action_info_t *action,
    size_t action_index,
    web_console_action_input_t *out_inputs,
    web_console_action_request_t *out_action_request)
{
    static const char *const root_keys[] = { "inputs" };
    static const char *const input_keys[] = { "id", "value" };
    if (!web_console_provider_object_has_keys(root, root_keys, 1U))
    {
        return WEB_CONSOLE_ACTION_PARSE_MALFORMED;
    }
    const cJSON *inputs = cJSON_GetObjectItemCaseSensitive(root, "inputs");
    if (!cJSON_IsArray(inputs))
    {
        return WEB_CONSOLE_ACTION_PARSE_MALFORMED;
    }
    const int input_count = cJSON_GetArraySize(inputs);
    if (input_count < 0 || (size_t) input_count > action->input_field_count
        || (size_t) input_count > WEB_CONSOLE_PROVIDER_MAX_FIELDS_PER_SECTION)
    {
        return WEB_CONSOLE_ACTION_PARSE_INVALID_INPUT;
    }

    bool seen_fields[WEB_CONSOLE_PROVIDER_MAX_FIELDS_PER_SECTION]{};
    for (int input_index = 0; input_index < input_count; ++input_index)
    {
        const cJSON *input = cJSON_GetArrayItem(inputs, input_index);
        if (!web_console_provider_object_has_keys(input, input_keys, 2U))
        {
            return WEB_CONSOLE_ACTION_PARSE_INVALID_INPUT;
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(input, "id");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(input, "value");
        if (!cJSON_IsString(id) || id->valuestring == NULL)
        {
            return WEB_CONSOLE_ACTION_PARSE_INVALID_INPUT;
        }
        size_t field_index = 0U;
        if (!web_console_provider_find_field(action->input_fields,
                                             action->input_field_count,
                                             id->valuestring,
                                             &field_index)
            || seen_fields[field_index])
        {
            return WEB_CONSOLE_ACTION_PARSE_INVALID_INPUT;
        }
        seen_fields[field_index] = true;
        const web_console_field_info_t *field = &action->input_fields[field_index];
        if ((field->access & WEB_CONSOLE_FIELD_ACCESS_SECRET) != 0U)
        {
            return WEB_CONSOLE_ACTION_PARSE_SECRET;
        }
        if (!web_console_provider_parse_change_value(
                field, value, &out_inputs[input_index].value))
        {
            return WEB_CONSOLE_ACTION_PARSE_INVALID_INPUT;
        }
        out_inputs[input_index].field_index = field_index;
    }

    out_action_request->action_index = action_index;
    out_action_request->inputs = input_count > 0 ? out_inputs : NULL;
    out_action_request->input_count = (size_t) input_count;
    return WEB_CONSOLE_ACTION_PARSE_OK;
}

/** @brief 把 Actions 同步拒绝映射为稳定 HTTP 状态和 reason。 */
static esp_err_t web_console_provider_send_action_rejection(httpd_req_t *request,
                                                            esp_err_t provider_error)
{
    if (provider_error == ESP_ERR_INVALID_ARG)
    {
        return web_console_provider_send_error(
            request,
            "422 Unprocessable Content",
            "{\"error\":\"action_rejected\",\"reason\":\"validation_failed\",\"message\":\"操作输入不满足约束\"}");
    }
    if (provider_error == ESP_ERR_INVALID_STATE)
    {
        return web_console_provider_send_error(
            request,
            "409 Conflict",
            "{\"error\":\"action_rejected\",\"reason\":\"owner_busy\",\"message\":\"当前状态暂不接受该操作\"}");
    }
    if (provider_error == ESP_ERR_TIMEOUT)
    {
        return web_console_provider_send_error(
            request,
            "504 Gateway Timeout",
            "{\"error\":\"action_rejected\",\"reason\":\"timeout\",\"message\":\"操作提交超时\"}");
    }
    return web_console_provider_send_error(
        request,
        "503 Service Unavailable",
        "{\"error\":\"action_rejected\",\"reason\":\"unknown\",\"message\":\"管理操作暂时不可用\"}");
}

/** @brief 接收、校验并异步提交一个非破坏性管理操作。 */
static esp_err_t web_console_provider_handle_action_post(httpd_req_t *request)
{
    bool authorized = false;
    esp_err_t error = web_console_provider_authorize(request, &authorized);
    if (!authorized)
    {
        return error;
    }

    char section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U]{};
    char action_id[WEB_CONSOLE_PROVIDER_FIELD_ID_MAX_LENGTH + 1U]{};
    size_t action_index = 0U;
    const web_console_action_provider_t *provider =
        web_console_provider_find_action_request(
            request, section, action_id, &action_index, &error);
    if (provider == NULL)
    {
        return error;
    }
    const web_console_action_info_t *action = &provider->actions[action_index];

    if (!web_console_provider_content_type_is_json(request))
    {
        return web_console_provider_send_error(
            request,
            "415 Unsupported Media Type",
            "{\"error\":\"unsupported_media_type\",\"message\":\"请求正文必须是 JSON\"}");
    }
    if (request->content_len == 0U)
    {
        return web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"请求正文不能为空\"}");
    }
    if (request->content_len > WEB_CONSOLE_PROVIDER_REQUEST_BODY_MAX_BYTES)
    {
        return web_console_provider_send_error(
            request,
            "413 Content Too Large",
            "{\"error\":\"payload_too_large\",\"message\":\"操作请求正文过大\"}");
    }

    bool io_failed = false;
    cJSON *root = web_console_provider_receive_json(request, &io_failed);
    if (root == NULL)
    {
        error = web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"操作请求 JSON 无效\"}");
        return io_failed ? ESP_FAIL : error;
    }

    web_console_action_input_t *inputs = NULL;
    if (action->input_field_count > 0U)
    {
        inputs = static_cast<web_console_action_input_t *>(
            calloc(action->input_field_count, sizeof(web_console_action_input_t)));
        if (inputs == NULL)
        {
            cJSON_Delete(root);
            return web_console_provider_send_error(
                request,
                "500 Internal Server Error",
                "{\"error\":\"internal_error\",\"message\":\"服务内部资源不足\"}");
        }
    }

    web_console_action_request_t action_request{};
    const web_console_action_parse_result_t parse_result =
        web_console_provider_parse_action_request(
            root, action, action_index, inputs, &action_request);
    cJSON_Delete(root);
    if (parse_result != WEB_CONSOLE_ACTION_PARSE_OK)
    {
        if (inputs != NULL)
        {
            web_console_provider_secure_clear(
                inputs, action->input_field_count * sizeof(web_console_action_input_t));
            free(inputs);
        }
        if (parse_result == WEB_CONSOLE_ACTION_PARSE_SECRET)
        {
            return web_console_provider_send_error(
                request,
                "403 Forbidden",
                "{\"error\":\"secret_input_forbidden\",\"message\":\"当前连接不允许提交秘密字段\"}");
        }
        return web_console_provider_send_error(
            request,
            parse_result == WEB_CONSOLE_ACTION_PARSE_MALFORMED
                ? "400 Bad Request"
                : "422 Unprocessable Content",
            parse_result == WEB_CONSOLE_ACTION_PARSE_MALFORMED
                ? "{\"error\":\"bad_request\",\"message\":\"操作请求结构无效\"}"
                : "{\"error\":\"invalid_input\",\"message\":\"操作字段或值不满足约束\"}");
    }

    const esp_err_t validation_error =
        provider->validate_request(provider->context, &action_request);
    if (validation_error != ESP_OK)
    {
        if (inputs != NULL)
        {
            web_console_provider_secure_clear(
                inputs, action->input_field_count * sizeof(web_console_action_input_t));
            free(inputs);
        }
        return web_console_provider_send_action_rejection(request, validation_error);
    }

    uint64_t request_id = 0U;
    const esp_err_t request_error =
        provider->request_copy(provider->context, &action_request, &request_id);
    if (inputs != NULL)
    {
        web_console_provider_secure_clear(
            inputs, action->input_field_count * sizeof(web_console_action_input_t));
        free(inputs);
    }
    if (request_error != ESP_OK)
    {
        return web_console_provider_send_action_rejection(request, request_error);
    }
    if (request_id == 0U)
    {
        return web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"provider_contract\",\"message\":\"操作提供者返回了无效请求标识\"}");
    }

    char request_text[WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE]{};
    cJSON *response = cJSON_CreateObject();
    if (!web_console_format_uint64(request_id, request_text) || response == NULL
        || cJSON_AddStringToObject(response, "section", provider->section_id) == NULL
        || cJSON_AddStringToObject(response, "action", action->id) == NULL
        || cJSON_AddStringToObject(response, "state", "pending") == NULL
        || cJSON_AddStringToObject(response, "reason", "none") == NULL
        || cJSON_AddStringToObject(response, "requestId", request_text) == NULL)
    {
        cJSON_Delete(response);
        web_console_provider_secure_clear(request_text, sizeof(request_text));
        return ESP_ERR_NO_MEM;
    }
    error = web_console_provider_send_cjson(request, "202 Accepted", response);
    cJSON_Delete(response);
    web_console_provider_secure_clear(request_text, sizeof(request_text));
    web_console_provider_secure_clear(action_id, sizeof(action_id));
    web_console_provider_secure_clear(section, sizeof(section));
    return error;
}

/** @brief 校验一次 Actions Provider 返回的当前结果。 */
static bool web_console_action_result_is_valid(const web_console_action_result_t *result)
{
    if (web_console_result_reason_name(result->reason) == NULL)
    {
        return false;
    }
    switch (result->state)
    {
        case WEB_CONSOLE_ACTION_STATE_PENDING:
        case WEB_CONSOLE_ACTION_STATE_SUCCEEDED:
            return result->reason == WEB_CONSOLE_RESULT_REASON_NONE;
        case WEB_CONSOLE_ACTION_STATE_FAILED:
            return result->reason != WEB_CONSOLE_RESULT_REASON_NONE;
        default:
            return false;
    }
}

/** @brief 返回异步管理操作的当前或最终结果。 */
static esp_err_t web_console_provider_handle_action_result_get(httpd_req_t *request)
{
    bool authorized = false;
    esp_err_t error = web_console_provider_authorize(request, &authorized);
    if (!authorized)
    {
        return error;
    }

    char section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U]{};
    char action_id[WEB_CONSOLE_PROVIDER_FIELD_ID_MAX_LENGTH + 1U]{};
    size_t action_index = 0U;
    const web_console_action_provider_t *provider =
        web_console_provider_find_action_request(
            request, section, action_id, &action_index, &error);
    if (provider == NULL)
    {
        return error;
    }

    char request_text[WEB_CONSOLE_PROVIDER_UINT64_TEXT_SIZE]{};
    uint64_t request_id = 0U;
    if (web_console_provider_get_query_value(
            request, "request", request_text, sizeof(request_text))
            != ESP_OK
        || !web_console_parse_uint64(request_text, &request_id) || request_id == 0U)
    {
        web_console_provider_secure_clear(request_text, sizeof(request_text));
        return web_console_provider_send_error(
            request,
            "400 Bad Request",
            "{\"error\":\"bad_request\",\"message\":\"缺少或无效的 request 参数\"}");
    }

    web_console_action_result_t result{};
    const esp_err_t provider_error = provider->get_result_copy(
        provider->context, action_index, request_id, &result);
    if (provider_error == ESP_ERR_NOT_FOUND)
    {
        error = web_console_provider_send_error(
            request,
            "404 Not Found",
            "{\"error\":\"request_not_found\",\"message\":\"管理操作请求不存在或已过期\"}");
    }
    else if (provider_error != ESP_OK)
    {
        error = web_console_provider_send_error(
            request,
            "503 Service Unavailable",
            "{\"error\":\"provider_unavailable\",\"message\":\"管理操作结果暂时不可读取\"}");
    }
    else if (!web_console_action_result_is_valid(&result))
    {
        error = web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"provider_contract\",\"message\":\"操作提供者返回了无效结果\"}");
    }
    else
    {
        const char *state = result.state == WEB_CONSOLE_ACTION_STATE_PENDING
                                ? "pending"
                            : result.state == WEB_CONSOLE_ACTION_STATE_SUCCEEDED
                                ? "succeeded"
                                : "failed";
        const char *reason = web_console_result_reason_name(result.reason);
        cJSON *response = cJSON_CreateObject();
        if (response == NULL
            || cJSON_AddStringToObject(response, "section", provider->section_id) == NULL
            || cJSON_AddStringToObject(response, "action", provider->actions[action_index].id) == NULL
            || cJSON_AddStringToObject(response, "request", request_text) == NULL
            || cJSON_AddStringToObject(response, "state", state) == NULL
            || cJSON_AddStringToObject(response, "reason", reason) == NULL)
        {
            error = ESP_ERR_NO_MEM;
        }
        else
        {
            error = web_console_provider_send_cjson(
                request,
                result.state == WEB_CONSOLE_ACTION_STATE_PENDING
                    ? "202 Accepted"
                    : "200 OK",
                response);
        }
        cJSON_Delete(response);
    }

    web_console_provider_secure_clear(request_text, sizeof(request_text));
    web_console_provider_secure_clear(action_id, sizeof(action_id));
    web_console_provider_secure_clear(section, sizeof(section));
    return error;
}
#endif
#endif

#if CONFIG_WEB_CONSOLE_STATUS
/** @brief 返回指定 Status 分区的一次完整运行摘要。 */
static esp_err_t web_console_provider_handle_status_get(httpd_req_t *request)
{
    bool      authorized = false;
    esp_err_t error      = web_console_provider_authorize(request, &authorized);
    if (!authorized)
    {
        return error;
    }

    char section[WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH + 1U]{};
    const web_console_status_provider_t *provider =
        web_console_provider_find_status_request(request, section, &error);
    if (provider == NULL)
    {
        return error;
    }

    web_console_field_value_t *values = static_cast<web_console_field_value_t *>(
        calloc(provider->field_count, sizeof(web_console_field_value_t)));
    if (values == NULL)
    {
        return web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"internal_error\",\"message\":\"服务内部资源不足\"}");
    }

    web_console_section_status_t status = {
        .version        = 0U,
        .values         = values,
        .value_capacity = provider->field_count,
        .value_count    = 0U,
    };
    const esp_err_t provider_error =
        provider->get_status_copy(provider->context, &status);
    if (provider_error != ESP_OK)
    {
        error = web_console_provider_send_error(
            request,
            "503 Service Unavailable",
            "{\"error\":\"provider_unavailable\",\"message\":\"状态暂时不可读取\"}");
    }
    else if (!web_console_status_is_valid(provider, &status, values))
    {
        error = web_console_provider_send_error(
            request,
            "500 Internal Server Error",
            "{\"error\":\"provider_contract\",\"message\":\"状态提供者返回了无效结果\"}");
    }
    else
    {
        error = web_console_provider_send_section_values(
            request,
            provider->section_id,
            status.version,
            provider->fields,
            values,
            provider->field_count);
    }

    web_console_provider_secure_clear(values,
                                      provider->field_count * sizeof(web_console_field_value_t));
    free(values);
    web_console_provider_secure_clear(section, sizeof(section));
    return error;
}
#endif

#if CONFIG_WEB_CONSOLE_SETTINGS || CONFIG_WEB_CONSOLE_STATUS || CONFIG_WEB_CONSOLE_ACTIONS
static constexpr web_console_route_t s_provider_routes[] = {
#if CONFIG_WEB_CONSOLE_SETTINGS
    { .uri = "/api/settings", .method = HTTP_GET, .handle = web_console_provider_handle_settings_get },
    { .uri = "/api/settings", .method = HTTP_PATCH, .handle = web_console_provider_handle_settings_patch },
    { .uri = "/api/settings/result",
      .method = HTTP_GET,
      .handle = web_console_provider_handle_settings_result_get },
#endif
#if CONFIG_WEB_CONSOLE_STATUS
    { .uri = "/api/status", .method = HTTP_GET, .handle = web_console_provider_handle_status_get },
#endif
#if CONFIG_WEB_CONSOLE_ACTIONS
    { .uri = "/api/actions", .method = HTTP_POST, .handle = web_console_provider_handle_action_post },
    { .uri = "/api/actions/result",
      .method = HTTP_GET,
      .handle = web_console_provider_handle_action_result_get },
#endif
};

static_assert(sizeof(s_provider_routes) / sizeof(s_provider_routes[0])
                  == WEB_CONSOLE_PROVIDER_ROUTE_COUNT,
              "Provider 路由数量必须与固定槽配置一致");
#endif

const web_console_route_t *web_console_provider_get_routes(size_t *out_count)
{
    if (out_count == NULL)
    {
        return NULL;
    }
#if CONFIG_WEB_CONSOLE_SETTINGS || CONFIG_WEB_CONSOLE_STATUS || CONFIG_WEB_CONSOLE_ACTIONS
    *out_count = sizeof(s_provider_routes) / sizeof(s_provider_routes[0]);
    return s_provider_routes;
#else
    *out_count = 0U;
    return NULL;
#endif
}
