#include <stddef.h>
#include <stdint.h>

#include "web_console_provider.h"

static esp_err_t get_settings_snapshot(
    void *context,
    web_console_settings_snapshot_t *out_snapshot)
{
    (void) context;
    (void) out_snapshot;
    return ESP_OK;
}

static esp_err_t validate_settings(
    void *context,
    const web_console_settings_update_t *update)
{
    (void) context;
    (void) update;
    return ESP_OK;
}

static esp_err_t request_settings(
    void *context,
    const web_console_settings_update_t *update,
    uint64_t *out_request_id)
{
    (void) context;
    (void) update;
    *out_request_id = 1U;
    return ESP_OK;
}

static esp_err_t get_settings_result(
    void *context,
    uint64_t request_id,
    web_console_settings_update_result_t *out_result)
{
    (void) context;
    (void) request_id;
    (void) out_result;
    return ESP_OK;
}

static esp_err_t get_status(
    void *context,
    web_console_section_status_t *out_status)
{
    (void) context;
    (void) out_status;
    return ESP_OK;
}

/* 341f3d3 公开头允许的旧式位置初始化器必须继续按原成员类型解释。 */
static const web_console_field_info_t legacy_field = {
    "legacy_field",
    "旧字段",
    WEB_CONSOLE_FIELD_TYPE_STRING,
    0U,
    WEB_CONSOLE_FIELD_EFFECT_IMMEDIATE,
    0,
    0,
    0U,
    31U,
    NULL,
    NULL,
    0U,
};

static const web_console_settings_provider_t legacy_settings_provider = {
    "legacy_settings",
    "旧设置",
    &legacy_field,
    1U,
    get_settings_snapshot,
    validate_settings,
    request_settings,
    get_settings_result,
    NULL,
};

static const web_console_status_provider_t legacy_status_provider = {
    "legacy_status",
    "旧状态",
    &legacy_field,
    1U,
    get_status,
    NULL,
};

typedef struct
{
    const char                         *id;
    const char                         *label;
    web_console_field_type_t            type;
    web_console_field_access_t          access;
    web_console_field_effect_t          effect;
    int64_t                             minimum;
    int64_t                             maximum;
    uint32_t                            step;
    uint32_t                            max_length_bytes;
    const char                         *file_suffix;
    const web_console_field_enum_value_t *enum_values;
    size_t                              enum_value_count;
} legacy_field_info_layout_t;

typedef struct
{
    const char                                      *section_id;
    const char                                      *label;
    const web_console_field_info_t                   *fields;
    size_t                                           field_count;
    web_console_settings_get_snapshot_copy_cb_t      get_snapshot_copy;
    web_console_settings_validate_update_cb_t        validate_update;
    web_console_settings_request_update_copy_cb_t    request_update_copy;
    web_console_settings_get_update_result_copy_cb_t get_update_result_copy;
    void                                            *context;
} legacy_settings_provider_layout_t;

typedef struct
{
    const char                             *section_id;
    const char                             *label;
    const web_console_field_info_t          *fields;
    size_t                                  field_count;
    web_console_status_get_status_copy_cb_t get_status_copy;
    void                                   *context;
} legacy_status_provider_layout_t;

typedef struct
{
    web_console_settings_update_state_t state;
    uint64_t                            version;
    esp_err_t                           error;
} legacy_settings_result_layout_t;

#define ASSERT_PREFIX_OFFSET(current_type, legacy_type, member) \
    _Static_assert(offsetof(current_type, member) == offsetof(legacy_type, member), \
                   #current_type "." #member " 不再保持旧前缀位置")

ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, id);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, label);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, type);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, access);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, effect);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, minimum);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, maximum);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, step);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, max_length_bytes);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, file_suffix);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, enum_values);
ASSERT_PREFIX_OFFSET(web_console_field_info_t, legacy_field_info_layout_t, enum_value_count);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, section_id);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, label);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, fields);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, field_count);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, get_snapshot_copy);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, validate_update);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, request_update_copy);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, get_update_result_copy);
ASSERT_PREFIX_OFFSET(web_console_settings_provider_t, legacy_settings_provider_layout_t, context);
ASSERT_PREFIX_OFFSET(web_console_status_provider_t, legacy_status_provider_layout_t, section_id);
ASSERT_PREFIX_OFFSET(web_console_status_provider_t, legacy_status_provider_layout_t, label);
ASSERT_PREFIX_OFFSET(web_console_status_provider_t, legacy_status_provider_layout_t, fields);
ASSERT_PREFIX_OFFSET(web_console_status_provider_t, legacy_status_provider_layout_t, field_count);
ASSERT_PREFIX_OFFSET(web_console_status_provider_t, legacy_status_provider_layout_t, get_status_copy);
ASSERT_PREFIX_OFFSET(web_console_status_provider_t, legacy_status_provider_layout_t, context);
ASSERT_PREFIX_OFFSET(web_console_settings_update_result_t, legacy_settings_result_layout_t, state);
ASSERT_PREFIX_OFFSET(web_console_settings_update_result_t, legacy_settings_result_layout_t, version);
ASSERT_PREFIX_OFFSET(web_console_settings_update_result_t, legacy_settings_result_layout_t, error);

#undef ASSERT_PREFIX_OFFSET

_Static_assert(offsetof(web_console_field_info_t, id)
                   < offsetof(web_console_field_info_t, label),
               "旧字段前缀顺序改变");
_Static_assert(offsetof(web_console_field_info_t, label)
                   < offsetof(web_console_field_info_t, type),
               "type 必须继续紧随旧 label 前缀");
_Static_assert(offsetof(web_console_field_info_t, enum_value_count)
                   < offsetof(web_console_field_info_t, description)
                   && offsetof(web_console_field_info_t, description) >= sizeof(legacy_field_info_layout_t),
               "新增字段元数据必须追加到旧前缀尾部");
_Static_assert(offsetof(web_console_settings_provider_t, label)
                   < offsetof(web_console_settings_provider_t, fields),
               "Settings 旧 fields 位置改变");
_Static_assert(offsetof(web_console_settings_provider_t, context)
                   < offsetof(web_console_settings_provider_t, description)
                   && offsetof(web_console_settings_provider_t, description)
                          >= sizeof(legacy_settings_provider_layout_t),
               "Settings description 必须追加到旧前缀尾部");
_Static_assert(offsetof(web_console_status_provider_t, label)
                   < offsetof(web_console_status_provider_t, fields),
               "Status 旧 fields 位置改变");
_Static_assert(offsetof(web_console_status_provider_t, context)
                   < offsetof(web_console_status_provider_t, description)
                   && offsetof(web_console_status_provider_t, description)
                          >= sizeof(legacy_status_provider_layout_t),
               "Status description 必须追加到旧前缀尾部");
_Static_assert(offsetof(web_console_settings_update_result_t, reason)
                   > offsetof(web_console_settings_update_result_t, error),
               "Settings reason 必须追加到旧结果前缀尾部");
_Static_assert(sizeof(((web_console_field_value_t *) 0)->data.string_value) == 128U,
               "STRING 按本功能要求保留 128 字节值缓冲区");

const void *web_console_legacy_compatibility_references[] = {
    &legacy_settings_provider,
    &legacy_status_provider,
};
