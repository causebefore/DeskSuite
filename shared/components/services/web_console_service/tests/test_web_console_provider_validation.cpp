#include <assert.h>
#include <stddef.h>

#include "web_console_provider_validation.hpp"

int main()
{
    static constexpr char chinese_path[] =
        "/\xE9\x9F\xB3\xE4\xB9\x90/\xE5\xAE\x8C\xE6\x88\x90.mp3";
    static constexpr char invalid_utf8[] = { '/', static_cast<char>(0xE4),
                                             static_cast<char>(0xB8), '.', 'm', 'p', '3' };
    static constexpr char embedded_nul[] = { 'a', '\0', 'b' };
    static constexpr char value_128[] =
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";

    assert(web_console_provider_utf8_string_value_is_valid(
        chinese_path, sizeof(chinese_path) - 1U, 127U));
    assert(!web_console_provider_utf8_string_value_is_valid(
        chinese_path, sizeof(chinese_path) - 1U, sizeof(chinese_path) - 2U));
    assert(!web_console_provider_utf8_string_value_is_valid(
        invalid_utf8, sizeof(invalid_utf8), 127U));
    assert(!web_console_provider_utf8_string_value_is_valid(
        embedded_nul, sizeof(embedded_nul), 127U));
    assert(!web_console_provider_utf8_string_value_is_valid(
        value_128, sizeof(value_128) - 1U, 127U));

    static constexpr char decoded_nul[] = R"({"value":"ok\u0000tail"})";
    static constexpr char literal_nul_escape[] = R"({"value":"ok\\u0000tail"})";
    assert(!web_console_provider_json_strings_have_no_nul_escape(
        decoded_nul, sizeof(decoded_nul) - 1U));
    assert(web_console_provider_json_strings_have_no_nul_escape(
        literal_nul_escape, sizeof(literal_nul_escape) - 1U));

    assert(web_console_provider_settings_result_is_valid(
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING,
        ESP_OK,
        WEB_CONSOLE_RESULT_REASON_NONE));
    assert(web_console_provider_settings_result_is_valid(
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_SUCCEEDED,
        ESP_OK,
        WEB_CONSOLE_RESULT_REASON_NONE));
    assert(web_console_provider_settings_result_is_valid(
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED,
        -1,
        WEB_CONSOLE_RESULT_REASON_UNKNOWN));
    assert(!web_console_provider_settings_result_is_valid(
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_PENDING,
        -1,
        WEB_CONSOLE_RESULT_REASON_TIMEOUT));
    assert(!web_console_provider_settings_result_is_valid(
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_SUCCEEDED,
        ESP_OK,
        WEB_CONSOLE_RESULT_REASON_VERSION_CONFLICT));
    assert(!web_console_provider_settings_result_is_valid(
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED,
        -1,
        WEB_CONSOLE_RESULT_REASON_NONE));
    assert(!web_console_provider_settings_result_is_valid(
        WEB_CONSOLE_SETTINGS_UPDATE_STATE_FAILED,
        ESP_OK,
        WEB_CONSOLE_RESULT_REASON_UNKNOWN));
    return 0;
}
