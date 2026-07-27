/**
 * @file protocol_backend_context.c
 * @brief 实现设备后端连接与身份上下文的校验和复制
 */
#include "protocol_backend_context.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/** @brief 检查普通字符串能否完整复制到固定容量缓冲区 */
static bool protocol_backend_string_fits(const char *value, size_t capacity, bool allow_empty)
{
    if (value == NULL || capacity == 0U)
    {
        return false;
    }
    for (size_t length = 0U; length < capacity; ++length)
    {
        if (value[length] == '\0')
        {
            return allow_empty || length > 0U;
        }
    }
    return false;
}

/** @brief 校验固件目标与 Hub 清单文件名约束一致 */
static bool protocol_backend_firmware_target_is_valid(const char *value)
{
    if (!protocol_backend_string_fits(value, PROTOCOL_BACKEND_FIRMWARE_TARGET_MAX, false) || value[0] < 'a'
        || value[0] > 'z')
    {
        return false;
    }
    for (const char *cursor = value + 1; *cursor != '\0'; ++cursor)
    {
        const bool lower = *cursor >= 'a' && *cursor <= 'z';
        const bool digit = *cursor >= '0' && *cursor <= '9';
        if (!lower && !digit && *cursor != '_')
        {
            return false;
        }
    }
    return true;
}

bool protocol_backend_context_is_valid(const protocol_backend_context_t *context)
{
    return context != NULL && protocol_backend_string_fits(context->base_url, sizeof(context->base_url), false)
           && protocol_backend_string_fits(context->token, sizeof(context->token), true)
           && protocol_backend_string_fits(context->device_id, sizeof(context->device_id), false)
           && context->product_id > 0U && protocol_backend_firmware_target_is_valid(context->firmware_target);
}

esp_err_t protocol_backend_context_build_copy(const protocol_backend_context_config_t *config,
                                              protocol_backend_context_t              *out_context)
{
    if (config == NULL || out_context == NULL
        || !protocol_backend_string_fits(config->base_url, PROTOCOL_BACKEND_BASE_URL_MAX, false)
        || !protocol_backend_string_fits(config->token != NULL ? config->token : "", PROTOCOL_BACKEND_TOKEN_MAX, true)
        || config->product_id == 0U || !protocol_backend_firmware_target_is_valid(config->firmware_target)
        || (config->device_id != NULL && config->device_id[0] != '\0'
            && !protocol_backend_string_fits(config->device_id, PROTOCOL_IDENTITY_DEVICE_ID_MAX, false)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    protocol_backend_context_t context = { 0 };
    (void) snprintf(context.base_url, sizeof(context.base_url), "%s", config->base_url);
    (void) snprintf(context.token, sizeof(context.token), "%s", config->token != NULL ? config->token : "");
    context.product_id = config->product_id;
    (void) snprintf(context.firmware_target, sizeof(context.firmware_target), "%s", config->firmware_target);

    esp_err_t error = ESP_OK;
    if (config->device_id != NULL && config->device_id[0] != '\0')
    {
        (void) snprintf(context.device_id, sizeof(context.device_id), "%s", config->device_id);
    }
    else
    {
        error = protocol_identity_get_hardware_device_id_copy(context.device_id, sizeof(context.device_id));
    }
    if (error != ESP_OK)
    {
        return error;
    }
    *out_context = context;
    return ESP_OK;
}
