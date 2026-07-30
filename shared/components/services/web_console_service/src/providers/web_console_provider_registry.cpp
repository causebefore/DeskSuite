/**
 * @file web_console_provider_registry.cpp
 * @brief Settings/Status Provider 元数据校验、深复制与固定发现实现
 */
#include "web_console_provider_internal.hpp"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct web_console_settings_provider_storage_t
{
    web_console_settings_provider_t provider;
    char                           *section_id;
    char                           *label;
    web_console_field_info_t       *fields;
};

struct web_console_status_provider_storage_t
{
    web_console_status_provider_t provider;
    char                         *section_id;
    char                         *label;
    web_console_field_info_t     *fields;
};

struct web_console_provider_registry_t
{
    web_console_settings_provider_storage_t settings[WEB_CONSOLE_SETTINGS_PROVIDER_MAX_COUNT];
    web_console_status_provider_storage_t   status[WEB_CONSOLE_STATUS_PROVIDER_MAX_COUNT];
    size_t                                  settings_count;
    size_t                                  status_count;
};

static web_console_provider_registry_t s_registry{};

bool web_console_provider_utf8_is_valid(const char *text, size_t length)
{
    if (length > 0U && text == NULL)
    {
        return false;
    }

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

/**
 * @brief 校验并计算调用方字符串长度
 *
 * @param[in] text 待校验字符串
 * @param[in] maximum 最大字节数，不含 NUL
 * @param[out] out_length 成功时返回字节数
 * @return true 非空且在上限内；false 为空、空串或缺少有界 NUL
 */
static bool web_console_get_bounded_string_length(const char *text, size_t maximum, size_t *out_length)
{
    if (text == NULL || out_length == NULL)
    {
        return false;
    }
    const size_t length = strnlen(text, maximum + 1U);
    if (length == 0U || length > maximum)
    {
        return false;
    }
    *out_length = length;
    return true;
}

/** @brief 校验稳定 ID 只包含小写 ASCII 字母、数字、下划线和连字符。 */
static bool web_console_id_is_valid(const char *id, size_t maximum)
{
    size_t length = 0U;
    if (!web_console_get_bounded_string_length(id, maximum, &length) || id[0] < 'a' || id[0] > 'z')
    {
        return false;
    }
    for (size_t index = 1U; index < length; ++index)
    {
        const char value = id[index];
        if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '-'))
        {
            return false;
        }
    }
    return true;
}

/** @brief 校验一个字段访问属性及类型专属约束。 */
static bool web_console_field_info_is_valid(const web_console_field_info_t *field, bool status_field)
{
    if (field == NULL || !web_console_id_is_valid(field->id, WEB_CONSOLE_PROVIDER_FIELD_ID_MAX_LENGTH))
    {
        return false;
    }

    size_t label_length = 0U;
    if (!web_console_get_bounded_string_length(field->label, WEB_CONSOLE_PROVIDER_LABEL_MAX_LENGTH, &label_length))
    {
        return false;
    }
    if (!web_console_provider_utf8_is_valid(field->label, label_length))
    {
        return false;
    }

    constexpr web_console_field_access_t allowed_access =
        WEB_CONSOLE_FIELD_ACCESS_READ_ONLY | WEB_CONSOLE_FIELD_ACCESS_SECRET | WEB_CONSOLE_FIELD_ACCESS_WRITE_ONLY;
    if ((field->access & ~allowed_access) != 0U
        || ((field->access & WEB_CONSOLE_FIELD_ACCESS_READ_ONLY) != 0U
            && (field->access & WEB_CONSOLE_FIELD_ACCESS_WRITE_ONLY) != 0U))
    {
        return false;
    }
    const bool read_only = (field->access & WEB_CONSOLE_FIELD_ACCESS_READ_ONLY) != 0U;
    const bool secret    = (field->access & WEB_CONSOLE_FIELD_ACCESS_SECRET) != 0U;
    if (secret && field->type != WEB_CONSOLE_FIELD_TYPE_STRING)
    {
        return false;
    }
    if (status_field)
    {
        if (field->access != WEB_CONSOLE_FIELD_ACCESS_READ_ONLY || field->effect != WEB_CONSOLE_FIELD_EFFECT_NONE)
        {
            return false;
        }
    }
    else if ((read_only && field->effect != WEB_CONSOLE_FIELD_EFFECT_NONE)
             || (!read_only
                 && (field->effect <= WEB_CONSOLE_FIELD_EFFECT_NONE
                     || field->effect > WEB_CONSOLE_FIELD_EFFECT_IDLE_ONLY)))
    {
        return false;
    }

    switch (field->type)
    {
        case WEB_CONSOLE_FIELD_TYPE_BOOL:
            return field->minimum == 0 && field->maximum == 0 && field->step == 0U
                   && field->max_length_bytes == 0U && field->enum_values == NULL
                   && field->enum_value_count == 0U;

        case WEB_CONSOLE_FIELD_TYPE_INT32:
            return field->minimum >= INT32_MIN && field->maximum <= INT32_MAX
                   && field->minimum <= field->maximum && field->step > 0U
                   && field->max_length_bytes == 0U && field->enum_values == NULL
                   && field->enum_value_count == 0U;

        case WEB_CONSOLE_FIELD_TYPE_UINT32:
            return field->minimum >= 0 && field->maximum <= UINT32_MAX
                   && field->minimum <= field->maximum && field->step > 0U
                   && field->max_length_bytes == 0U && field->enum_values == NULL
                   && field->enum_value_count == 0U;

        case WEB_CONSOLE_FIELD_TYPE_STRING:
            return field->minimum == 0 && field->maximum == 0 && field->step == 0U
                   && field->max_length_bytes > 0U
                   && field->max_length_bytes <= WEB_CONSOLE_PROVIDER_STRING_MAX_LENGTH
                   && field->enum_values == NULL && field->enum_value_count == 0U;

        case WEB_CONSOLE_FIELD_TYPE_ENUM:
            if (field->minimum != 0 || field->maximum != 0 || field->step != 0U
                || field->max_length_bytes != 0U || field->enum_values == NULL
                || field->enum_value_count == 0U
                || field->enum_value_count > WEB_CONSOLE_PROVIDER_MAX_ENUM_VALUES)
            {
                return false;
            }
            for (size_t index = 0U; index < field->enum_value_count; ++index)
            {
                size_t enum_label_length = 0U;
                if (!web_console_get_bounded_string_length(field->enum_values[index].label,
                                                           WEB_CONSOLE_PROVIDER_LABEL_MAX_LENGTH,
                                                           &enum_label_length))
                {
                    return false;
                }
                if (!web_console_provider_utf8_is_valid(field->enum_values[index].label,
                                                        enum_label_length))
                {
                    return false;
                }
                for (size_t compared = 0U; compared < index; ++compared)
                {
                    if (field->enum_values[compared].value == field->enum_values[index].value)
                    {
                        return false;
                    }
                }
            }
            return true;

        default:
            return false;
    }
}

/** @brief 为一个已经校验的有界字符串创建精确长度副本。 */
static esp_err_t web_console_copy_string(const char *source, char **out_copy)
{
    const size_t length = strlen(source);
    char        *copy   = static_cast<char *>(malloc(length + 1U));
    if (copy == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, source, length + 1U);
    *out_copy = copy;
    return ESP_OK;
}

/** @brief 释放一个字段描述符的全部深复制字符串和枚举表。 */
static void web_console_free_field(web_console_field_info_t *field)
{
    if (field == NULL)
    {
        return;
    }
    web_console_field_enum_value_t *enum_values =
        const_cast<web_console_field_enum_value_t *>(field->enum_values);
    if (enum_values != NULL)
    {
        for (size_t index = 0U; index < field->enum_value_count; ++index)
        {
            free(const_cast<char *>(enum_values[index].label));
        }
        free(enum_values);
    }
    free(const_cast<char *>(field->id));
    free(const_cast<char *>(field->label));
    memset(field, 0, sizeof(*field));
}

/** @brief 释放一个动态字段描述符数组。 */
static void web_console_free_fields(web_console_field_info_t *fields, size_t field_count)
{
    if (fields == NULL)
    {
        return;
    }
    for (size_t index = 0U; index < field_count; ++index)
    {
        web_console_free_field(&fields[index]);
    }
    free(fields);
}

/** @brief 深复制一个已经校验的字段描述符。 */
static esp_err_t web_console_copy_field(web_console_field_info_t *destination,
                                        const web_console_field_info_t *source)
{
    *destination             = *source;
    destination->id          = NULL;
    destination->label       = NULL;
    destination->enum_values = NULL;

    char     *id_copy = NULL;
    esp_err_t error   = web_console_copy_string(source->id, &id_copy);
    destination->id   = id_copy;
    if (error == ESP_OK)
    {
        char *label_copy = NULL;
        error            = web_console_copy_string(source->label, &label_copy);
        destination->label = label_copy;
    }
    if (error == ESP_OK && source->enum_value_count > 0U)
    {
        web_console_field_enum_value_t *enum_values = static_cast<web_console_field_enum_value_t *>(
            calloc(source->enum_value_count, sizeof(web_console_field_enum_value_t)));
        if (enum_values == NULL)
        {
            error = ESP_ERR_NO_MEM;
        }
        else
        {
            destination->enum_values = enum_values;
            for (size_t index = 0U; error == ESP_OK && index < source->enum_value_count; ++index)
            {
                enum_values[index].value = source->enum_values[index].value;
                char *enum_label_copy = NULL;
                error = web_console_copy_string(source->enum_values[index].label, &enum_label_copy);
                enum_values[index].label = enum_label_copy;
            }
        }
    }
    if (error != ESP_OK)
    {
        web_console_free_field(destination);
    }
    return error;
}

/** @brief 校验同一分区中的字段 ID 唯一性。 */
static bool web_console_fields_are_valid(const web_console_field_info_t *fields,
                                         size_t field_count,
                                         bool status_fields)
{
    if (fields == NULL || field_count == 0U || field_count > WEB_CONSOLE_PROVIDER_MAX_FIELDS_PER_SECTION)
    {
        return false;
    }
    for (size_t index = 0U; index < field_count; ++index)
    {
        if (!web_console_field_info_is_valid(&fields[index], status_fields))
        {
            return false;
        }
        for (size_t compared = 0U; compared < index; ++compared)
        {
            if (strcmp(fields[index].id, fields[compared].id) == 0)
            {
                return false;
            }
        }
    }
    return true;
}

/** @brief 校验 Settings Provider 集合中的分区 ID、字段和回调。 */
static bool web_console_settings_providers_are_valid(const web_console_settings_provider_t *providers,
                                                     size_t count)
{
    if ((count == 0U) != (providers == NULL) || count > WEB_CONSOLE_SETTINGS_PROVIDER_MAX_COUNT)
    {
        return false;
    }
    for (size_t index = 0U; index < count; ++index)
    {
        const web_console_settings_provider_t *provider = &providers[index];
        size_t label_length = 0U;
        if (!web_console_id_is_valid(provider->section_id, WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH)
            || !web_console_get_bounded_string_length(provider->label,
                                                      WEB_CONSOLE_PROVIDER_LABEL_MAX_LENGTH,
                                                      &label_length)
            || !web_console_fields_are_valid(provider->fields, provider->field_count, false)
            || provider->get_snapshot_copy == NULL || provider->validate_update == NULL
            || provider->request_update_copy == NULL || provider->get_update_result_copy == NULL)
        {
            return false;
        }
        if (!web_console_provider_utf8_is_valid(provider->label, label_length))
        {
            return false;
        }
        for (size_t compared = 0U; compared < index; ++compared)
        {
            if (strcmp(provider->section_id, providers[compared].section_id) == 0)
            {
                return false;
            }
        }
    }
    return true;
}

/** @brief 校验 Status Provider 集合中的分区 ID、字段和回调。 */
static bool web_console_status_providers_are_valid(const web_console_status_provider_t *providers,
                                                   size_t count)
{
    if ((count == 0U) != (providers == NULL) || count > WEB_CONSOLE_STATUS_PROVIDER_MAX_COUNT)
    {
        return false;
    }
    for (size_t index = 0U; index < count; ++index)
    {
        const web_console_status_provider_t *provider = &providers[index];
        size_t label_length = 0U;
        if (!web_console_id_is_valid(provider->section_id, WEB_CONSOLE_PROVIDER_SECTION_ID_MAX_LENGTH)
            || !web_console_get_bounded_string_length(provider->label,
                                                      WEB_CONSOLE_PROVIDER_LABEL_MAX_LENGTH,
                                                      &label_length)
            || !web_console_fields_are_valid(provider->fields, provider->field_count, true)
            || provider->get_status_copy == NULL)
        {
            return false;
        }
        if (!web_console_provider_utf8_is_valid(provider->label, label_length))
        {
            return false;
        }
        for (size_t compared = 0U; compared < index; ++compared)
        {
            if (strcmp(provider->section_id, providers[compared].section_id) == 0)
            {
                return false;
            }
        }
    }
    return true;
}

/** @brief 深复制一个分区的字段描述符数组。 */
static esp_err_t web_console_copy_fields(const web_console_field_info_t *source,
                                         size_t field_count,
                                         web_console_field_info_t **out_fields)
{
    web_console_field_info_t *fields = static_cast<web_console_field_info_t *>(
        calloc(field_count, sizeof(web_console_field_info_t)));
    if (fields == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0U; index < field_count; ++index)
    {
        const esp_err_t error = web_console_copy_field(&fields[index], &source[index]);
        if (error != ESP_OK)
        {
            web_console_free_fields(fields, field_count);
            return error;
        }
    }
    *out_fields = fields;
    return ESP_OK;
}

/** @brief 释放一个 Settings Provider 的深复制元数据。 */
static void web_console_free_settings_provider(web_console_settings_provider_storage_t *storage)
{
    web_console_free_fields(storage->fields, storage->provider.field_count);
    free(storage->section_id);
    free(storage->label);
    memset(storage, 0, sizeof(*storage));
}

/** @brief 释放一个 Status Provider 的深复制元数据。 */
static void web_console_free_status_provider(web_console_status_provider_storage_t *storage)
{
    web_console_free_fields(storage->fields, storage->provider.field_count);
    free(storage->section_id);
    free(storage->label);
    memset(storage, 0, sizeof(*storage));
}

esp_err_t web_console_provider_registry_configure_copy(
    const web_console_settings_provider_t *settings_providers,
    size_t settings_provider_count,
    const web_console_status_provider_t *status_providers,
    size_t status_provider_count)
{
    if (!web_console_settings_providers_are_valid(settings_providers, settings_provider_count)
        || !web_console_status_providers_are_valid(status_providers, status_provider_count))
    {
        return ESP_ERR_INVALID_ARG;
    }

    web_console_provider_registry_reset();
    for (size_t index = 0U; index < settings_provider_count; ++index)
    {
        const web_console_settings_provider_t *source = &settings_providers[index];
        web_console_settings_provider_storage_t *destination = &s_registry.settings[index];
        destination->provider = *source;
        destination->provider.section_id = NULL;
        destination->provider.label      = NULL;
        destination->provider.fields     = NULL;

        esp_err_t error = web_console_copy_string(source->section_id, &destination->section_id);
        if (error == ESP_OK)
        {
            error = web_console_copy_string(source->label, &destination->label);
        }
        if (error == ESP_OK)
        {
            error = web_console_copy_fields(source->fields, source->field_count, &destination->fields);
        }
        if (error != ESP_OK)
        {
            web_console_provider_registry_reset();
            return error;
        }
        destination->provider.section_id = destination->section_id;
        destination->provider.label      = destination->label;
        destination->provider.fields     = destination->fields;
        s_registry.settings_count        = index + 1U;
    }
    for (size_t index = 0U; index < status_provider_count; ++index)
    {
        const web_console_status_provider_t *source = &status_providers[index];
        web_console_status_provider_storage_t *destination = &s_registry.status[index];
        destination->provider = *source;
        destination->provider.section_id = NULL;
        destination->provider.label      = NULL;
        destination->provider.fields     = NULL;

        esp_err_t error = web_console_copy_string(source->section_id, &destination->section_id);
        if (error == ESP_OK)
        {
            error = web_console_copy_string(source->label, &destination->label);
        }
        if (error == ESP_OK)
        {
            error = web_console_copy_fields(source->fields, source->field_count, &destination->fields);
        }
        if (error != ESP_OK)
        {
            web_console_provider_registry_reset();
            return error;
        }
        destination->provider.section_id = destination->section_id;
        destination->provider.label      = destination->label;
        destination->provider.fields     = destination->fields;
        s_registry.status_count          = index + 1U;
    }
    return ESP_OK;
}

void web_console_provider_registry_reset(void)
{
    for (size_t index = 0U; index < WEB_CONSOLE_SETTINGS_PROVIDER_MAX_COUNT; ++index)
    {
        web_console_free_settings_provider(&s_registry.settings[index]);
    }
    for (size_t index = 0U; index < WEB_CONSOLE_STATUS_PROVIDER_MAX_COUNT; ++index)
    {
        web_console_free_status_provider(&s_registry.status[index]);
    }
    memset(&s_registry, 0, sizeof(s_registry));
}

size_t web_console_provider_registry_get_settings_count(void)
{
    return s_registry.settings_count;
}

const web_console_settings_provider_t *web_console_provider_registry_get_settings(size_t index)
{
    return index < s_registry.settings_count ? &s_registry.settings[index].provider : NULL;
}

const web_console_settings_provider_t *web_console_provider_registry_find_settings(const char *section_id)
{
    if (section_id == NULL)
    {
        return NULL;
    }
    for (size_t index = 0U; index < s_registry.settings_count; ++index)
    {
        if (strcmp(section_id, s_registry.settings[index].provider.section_id) == 0)
        {
            return &s_registry.settings[index].provider;
        }
    }
    return NULL;
}

size_t web_console_provider_registry_get_status_count(void)
{
    return s_registry.status_count;
}

const web_console_status_provider_t *web_console_provider_registry_get_status(size_t index)
{
    return index < s_registry.status_count ? &s_registry.status[index].provider : NULL;
}

const web_console_status_provider_t *web_console_provider_registry_find_status(const char *section_id)
{
    if (section_id == NULL)
    {
        return NULL;
    }
    for (size_t index = 0U; index < s_registry.status_count; ++index)
    {
        if (strcmp(section_id, s_registry.status[index].provider.section_id) == 0)
        {
            return &s_registry.status[index].provider;
        }
    }
    return NULL;
}
