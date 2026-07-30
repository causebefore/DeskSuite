/**
 * @file web_console_network_provider.c
 * @brief 把 Network Manager 诊断快照映射为网页控制台只读状态
 */
#include "web_console_network_provider.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "network_manager.h"

typedef enum
{
    NETWORK_FIELD_MANAGER_STATE = 0,
    NETWORK_FIELD_MANAGER_LAST_ERROR,
    NETWORK_FIELD_LINK_SNAPSHOT_ERROR,
    NETWORK_FIELD_ASSOCIATED,
    NETWORK_FIELD_BSSID,
    NETWORK_FIELD_PRIMARY_CHANNEL,
    NETWORK_FIELD_RSSI_DBM,
    NETWORK_FIELD_IPV4,
    NETWORK_FIELD_GATEWAY,
    NETWORK_FIELD_DNS_PRIMARY,
    NETWORK_FIELD_HAS_SAVED_CONFIG,
    NETWORK_FIELD_PORTAL_ACTIVE,
    NETWORK_FIELD_COUNT,
} web_console_network_field_index_t;

#define NETWORK_FIELD_READ_ONLY WEB_CONSOLE_FIELD_ACCESS_READ_ONLY

static const web_console_field_enum_value_t s_manager_state_values[] = {
    { .value = NETWORK_STATE_STOPPED, .label = "已停止" },
    { .value = NETWORK_STATE_CONNECTING, .label = "连接中" },
    { .value = NETWORK_STATE_ONLINE, .label = "已联网" },
    { .value = NETWORK_STATE_RETRY_WAIT, .label = "等待重试" },
    { .value = NETWORK_STATE_PROVISIONING, .label = "配网中" },
    { .value = NETWORK_STATE_VALIDATING, .label = "验证配置" },
    { .value = NETWORK_STATE_ERROR, .label = "错误" },
    { .value = NETWORK_STATE_STOPPING, .label = "停止中" },
};

static const web_console_field_info_t s_network_fields[NETWORK_FIELD_COUNT] = {
    [NETWORK_FIELD_MANAGER_STATE] =
        {
            .id               = "manager_state",
            .label            = "管理器状态",
            .type             = WEB_CONSOLE_FIELD_TYPE_ENUM,
            .access           = NETWORK_FIELD_READ_ONLY,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .enum_values      = s_manager_state_values,
            .enum_value_count = sizeof(s_manager_state_values) / sizeof(s_manager_state_values[0]),
        },
    [NETWORK_FIELD_MANAGER_LAST_ERROR] =
        {
            .id      = "manager_last_error",
            .label   = "当前状态错误码",
            .type    = WEB_CONSOLE_FIELD_TYPE_INT32,
            .access  = NETWORK_FIELD_READ_ONLY,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .minimum = INT32_MIN,
            .maximum = INT32_MAX,
            .step    = 1U,
        },
    [NETWORK_FIELD_LINK_SNAPSHOT_ERROR] =
        {
            .id      = "link_snapshot_error",
            .label   = "链路查询错误码",
            .type    = WEB_CONSOLE_FIELD_TYPE_INT32,
            .access  = NETWORK_FIELD_READ_ONLY,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .minimum = INT32_MIN,
            .maximum = INT32_MAX,
            .step    = 1U,
        },
    [NETWORK_FIELD_ASSOCIATED] =
        {
            .id     = "associated",
            .label  = "已关联 AP",
            .type   = WEB_CONSOLE_FIELD_TYPE_BOOL,
            .access = NETWORK_FIELD_READ_ONLY,
            .effect = WEB_CONSOLE_FIELD_EFFECT_NONE,
        },
    [NETWORK_FIELD_BSSID] =
        {
            .id               = "bssid",
            .label            = "AP BSSID",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = NETWORK_FIELD_READ_ONLY,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .max_length_bytes = 17U,
        },
    [NETWORK_FIELD_PRIMARY_CHANNEL] =
        {
            .id      = "primary_channel",
            .label   = "主信道",
            .type    = WEB_CONSOLE_FIELD_TYPE_UINT32,
            .access  = NETWORK_FIELD_READ_ONLY,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .minimum = 0,
            .maximum = UINT8_MAX,
            .step    = 1U,
        },
    [NETWORK_FIELD_RSSI_DBM] =
        {
            .id      = "rssi_dbm",
            .label   = "RSSI（dBm）",
            .type    = WEB_CONSOLE_FIELD_TYPE_INT32,
            .access  = NETWORK_FIELD_READ_ONLY,
            .effect  = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .minimum = INT8_MIN,
            .maximum = INT8_MAX,
            .step    = 1U,
        },
    [NETWORK_FIELD_IPV4] =
        {
            .id               = "ipv4",
            .label            = "IPv4 地址",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = NETWORK_FIELD_READ_ONLY,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .max_length_bytes = 15U,
        },
    [NETWORK_FIELD_GATEWAY] =
        {
            .id               = "gateway",
            .label            = "IPv4 网关",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = NETWORK_FIELD_READ_ONLY,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .max_length_bytes = 15U,
        },
    [NETWORK_FIELD_DNS_PRIMARY] =
        {
            .id               = "dns_primary",
            .label            = "主 DNS",
            .type             = WEB_CONSOLE_FIELD_TYPE_STRING,
            .access           = NETWORK_FIELD_READ_ONLY,
            .effect           = WEB_CONSOLE_FIELD_EFFECT_NONE,
            .max_length_bytes = 15U,
        },
    [NETWORK_FIELD_HAS_SAVED_CONFIG] =
        {
            .id     = "has_saved_config",
            .label  = "已有持久化配置",
            .type   = WEB_CONSOLE_FIELD_TYPE_BOOL,
            .access = NETWORK_FIELD_READ_ONLY,
            .effect = WEB_CONSOLE_FIELD_EFFECT_NONE,
        },
    [NETWORK_FIELD_PORTAL_ACTIVE] =
        {
            .id     = "portal_active",
            .label  = "配网 Portal 活动",
            .type   = WEB_CONSOLE_FIELD_TYPE_BOOL,
            .access = NETWORK_FIELD_READ_ONLY,
            .effect = WEB_CONSOLE_FIELD_EFFECT_NONE,
        },
};

/** @brief 写入一个已配置的布尔状态字段。 */
static void write_bool_value(web_console_field_value_t *out_value, bool value)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type               = WEB_CONSOLE_FIELD_TYPE_BOOL;
    out_value->configured         = true;
    out_value->data.boolean_value = value;
}

/** @brief 写入一个已配置的有符号整数状态字段。 */
static void write_int32_value(web_console_field_value_t *out_value, int32_t value)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type             = WEB_CONSOLE_FIELD_TYPE_INT32;
    out_value->configured       = true;
    out_value->data.int32_value = value;
}

/** @brief 写入一个已配置的无符号整数状态字段。 */
static void write_uint32_value(web_console_field_value_t *out_value, uint32_t value)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type              = WEB_CONSOLE_FIELD_TYPE_UINT32;
    out_value->configured        = true;
    out_value->data.uint32_value = value;
}

/** @brief 写入一个已配置的枚举状态字段。 */
static void write_enum_value(web_console_field_value_t *out_value, int32_t value)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type             = WEB_CONSOLE_FIELD_TYPE_ENUM;
    out_value->configured       = true;
    out_value->data.int32_value = value;
}

/** @brief 有界复制一个由 Network Manager 保证 NUL 结尾的字符串字段。 */
static void write_string_value(web_console_field_value_t *out_value, const char *value)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type       = WEB_CONSOLE_FIELD_TYPE_STRING;
    out_value->configured = true;
    (void) snprintf(out_value->data.string_value, sizeof(out_value->data.string_value), "%s", value);
}

/** @brief 把二进制 BSSID 格式化为固定长度文本；未关联时返回空字符串。 */
static void write_bssid_value(web_console_field_value_t *out_value, const connect_link_info_t *link)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type       = WEB_CONSOLE_FIELD_TYPE_STRING;
    out_value->configured = true;
    if (!link->associated)
    {
        return;
    }

    (void) snprintf(
        out_value->data.string_value,
        sizeof(out_value->data.string_value),
        "%02x:%02x:%02x:%02x:%02x:%02x",
        (unsigned int) link->bssid[0],
        (unsigned int) link->bssid[1],
        (unsigned int) link->bssid[2],
        (unsigned int) link->bssid[3],
        (unsigned int) link->bssid[4],
        (unsigned int) link->bssid[5]);
}

/**
 * @brief 读取一次完整网络诊断并按固定字段顺序映射
 *
 * `network_manager_get_diagnostics_copy()` 是本回调唯一的事实来源；它会把链路查询失败单独写入
 * `link_snapshot_error`，因此本适配层仍返回完整、可诊断的其余 Manager 事实。
 */
static esp_err_t get_network_status_copy(void *context, web_console_section_status_t *out_status)
{
    (void) context;
    if (out_status == NULL || out_status->values == NULL || out_status->value_capacity < NETWORK_FIELD_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    network_manager_diagnostics_t diagnostics;
    const esp_err_t error = network_manager_get_diagnostics_copy(&diagnostics);
    if (error != ESP_OK)
    {
        return error;
    }
    if ((uint32_t) diagnostics.status.state > (uint32_t) NETWORK_STATE_STOPPING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    write_enum_value(
        &out_status->values[NETWORK_FIELD_MANAGER_STATE],
        (int32_t) diagnostics.status.state);
    write_int32_value(
        &out_status->values[NETWORK_FIELD_MANAGER_LAST_ERROR],
        (int32_t) diagnostics.status.last_error);
    write_int32_value(
        &out_status->values[NETWORK_FIELD_LINK_SNAPSHOT_ERROR],
        (int32_t) diagnostics.link_snapshot_error);
    write_bool_value(&out_status->values[NETWORK_FIELD_ASSOCIATED], diagnostics.link.associated);
    write_bssid_value(&out_status->values[NETWORK_FIELD_BSSID], &diagnostics.link);
    write_uint32_value(
        &out_status->values[NETWORK_FIELD_PRIMARY_CHANNEL],
        diagnostics.link.primary_channel);
    write_int32_value(&out_status->values[NETWORK_FIELD_RSSI_DBM], diagnostics.link.rssi_dbm);
    write_string_value(&out_status->values[NETWORK_FIELD_IPV4], diagnostics.link.ip);
    write_string_value(&out_status->values[NETWORK_FIELD_GATEWAY], diagnostics.link.gateway);
    write_string_value(&out_status->values[NETWORK_FIELD_DNS_PRIMARY], diagnostics.link.dns_primary);
    write_bool_value(
        &out_status->values[NETWORK_FIELD_HAS_SAVED_CONFIG],
        diagnostics.has_saved_config);
    write_bool_value(&out_status->values[NETWORK_FIELD_PORTAL_ACTIVE], diagnostics.portal_active);

    out_status->version     = 0U;
    out_status->value_count = NETWORK_FIELD_COUNT;
    return ESP_OK;
}

static const web_console_status_provider_t s_network_status_provider = {
    .section_id      = "network",
    .label           = "网络状态",
    .fields          = s_network_fields,
    .field_count     = NETWORK_FIELD_COUNT,
    .get_status_copy = get_network_status_copy,
    .context         = NULL,
};

const web_console_status_provider_t *web_console_network_provider_get_status_borrow(void)
{
    return &s_network_status_provider;
}
