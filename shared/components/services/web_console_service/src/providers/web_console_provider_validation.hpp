/**
 * @file web_console_provider_validation.hpp
 * @brief Provider UTF-8 STRING 与原始 JSON NUL escape 的纯校验 helper
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "web_console_provider.h"

/** @brief 校验精确字节序列是规范 UTF-8，且序列中不含 NUL。 */
constexpr bool web_console_provider_utf8_is_valid(const char *text, size_t length)
{
    if (length > 0U && text == nullptr)
    {
        return false;
    }

    size_t index = 0U;
    while (index < length)
    {
        const uint8_t first = static_cast<uint8_t>(text[index]);
        if (first == 0U)
        {
            return false;
        }
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

        const uint8_t second = static_cast<uint8_t>(text[index + 1U]);
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
            if ((static_cast<uint8_t>(text[index + offset]) & 0xC0U) != 0x80U)
            {
                return false;
            }
        }
        index += sequence_length;
    }
    return true;
}

/** @brief 校验 STRING 值满足字段上限、全局 127 字节上限和 UTF-8 契约。 */
constexpr bool web_console_provider_utf8_string_value_is_valid(
    const char *text,
    size_t length,
    size_t maximum_length)
{
    return length <= maximum_length && length <= WEB_CONSOLE_PROVIDER_STRING_MAX_LENGTH
           && web_console_provider_utf8_is_valid(text, length);
}

/** @brief 校验 Settings 异步状态、错误码和稳定原因的组合。 */
constexpr bool web_console_provider_settings_result_is_valid(
    web_console_settings_update_state_t state,
    esp_err_t error,
    web_console_result_reason_t reason)
{
    if (reason < WEB_CONSOLE_RESULT_REASON_NONE
        || reason > WEB_CONSOLE_RESULT_REASON_UNKNOWN)
    {
        return false;
    }
    switch (state)
    {
        case WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING:
        case WEB_CONSOLE_SETTINGS_UPDATE_STATE_SUCCEEDED:
            return error == ESP_OK && reason == WEB_CONSOLE_RESULT_REASON_NONE;
        case WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED:
            return error != ESP_OK && reason != WEB_CONSOLE_RESULT_REASON_NONE;
        default:
            return false;
    }
}

/** @brief 线性扫描 JSON string，拒绝会被解码为 NUL 的 `\u0000` escape。 */
constexpr bool web_console_provider_json_strings_have_no_nul_escape(
    const char *body,
    size_t body_size)
{
    bool in_string = false;
    bool escaped   = false;
    for (size_t index = 0U; index < body_size; ++index)
    {
        const char value = body[index];
        if (!in_string)
        {
            if (value == '"')
            {
                in_string = true;
            }
            continue;
        }
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
    }
    return true;
}
