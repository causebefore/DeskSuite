/*
 * 文件职责：纯解析并规范化 DeskMate Hub 的 HTTP authority 地址。
 */
#include "app_network_hub_url.h"

#include <stdbool.h>
#include <stddef.h>

static char ascii_to_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char) (value + ('a' - 'A')) : value;
}

static bool ascii_equals_ignore_case(const char *value, const char *expected, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        if (ascii_to_lower(value[index]) != expected[index])
        {
            return false;
        }
    }
    return true;
}

static bool host_is_ipv4_candidate(const char *host, size_t host_length)
{
    for (size_t index = 0U; index < host_length; ++index)
    {
        if ((host[index] < '0' || host[index] > '9') && host[index] != '.')
        {
            return false;
        }
    }
    return true;
}

static bool ipv4_is_valid(const char *host, size_t host_length)
{
    size_t index  = 0U;
    size_t octets = 0U;
    while (index < host_length)
    {
        if (octets >= 4U || host[index] < '0' || host[index] > '9')
        {
            return false;
        }
        unsigned value  = 0U;
        size_t   digits = 0U;
        while (index < host_length && host[index] >= '0' && host[index] <= '9')
        {
            value = value * 10U + (unsigned) (host[index] - '0');
            if (++digits > 3U || value > 255U)
            {
                return false;
            }
            ++index;
        }
        ++octets;
        if (index == host_length)
        {
            break;
        }
        if (host[index] != '.' || ++index == host_length)
        {
            return false;
        }
    }
    return octets == 4U;
}

static bool hostname_is_valid(const char *host, size_t host_length)
{
    size_t label_length = 0U;
    for (size_t index = 0U; index < host_length; ++index)
    {
        const char value = host[index];
        if (value == '.')
        {
            if (label_length == 0U || host[index - 1U] == '-')
            {
                return false;
            }
            label_length = 0U;
            continue;
        }
        const bool alphanumeric = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                                  || (value >= '0' && value <= '9');
        if ((!alphanumeric && value != '-') || (label_length == 0U && value == '-') || ++label_length > 63U)
        {
            return false;
        }
    }
    return label_length > 0U && host[host_length - 1U] != '-';
}

static bool port_is_valid(const char *port, size_t port_length)
{
    if (port_length == 0U || port_length > 5U)
    {
        return false;
    }
    unsigned value = 0U;
    for (size_t index = 0U; index < port_length; ++index)
    {
        if (port[index] < '0' || port[index] > '9')
        {
            return false;
        }
        value = value * 10U + (unsigned) (port[index] - '0');
    }
    return value > 0U && value <= 65535U;
}

esp_err_t app_network_hub_url_parse_copy(
    const char *input,
    char out_url[APP_NETWORK_HUB_URL_MAX_LENGTH + 1U])
{
    if (out_url == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    out_url[0] = '\0';
    if (input == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t input_length = 0U;
    while (input_length <= APP_NETWORK_HUB_URL_MAX_LENGTH + 1U && input[input_length] != '\0')
    {
        const unsigned char value = (unsigned char) input[input_length];
        if (value <= 0x20U || value >= 0x7FU)
        {
            return ESP_ERR_INVALID_ARG;
        }
        ++input_length;
    }
    if (input_length > APP_NETWORK_HUB_URL_MAX_LENGTH + 1U)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    static const char scheme[] = "http://";
    const size_t scheme_length = sizeof(scheme) - 1U;
    if (input_length <= scheme_length || !ascii_equals_ignore_case(input, scheme, scheme_length))
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t authority_end = input_length;
    if (input[authority_end - 1U] == '/')
    {
        --authority_end;
    }
    if (authority_end <= scheme_length || authority_end > APP_NETWORK_HUB_URL_MAX_LENGTH)
    {
        return authority_end > APP_NETWORK_HUB_URL_MAX_LENGTH ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }

    size_t host_end  = authority_end;
    size_t colon_pos = authority_end;
    for (size_t index = scheme_length; index < authority_end; ++index)
    {
        const char value = input[index];
        if (value == '/' || value == '?' || value == '#' || value == '@' || value == '[' || value == ']')
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (value == ':')
        {
            if (colon_pos != authority_end)
            {
                return ESP_ERR_INVALID_ARG;
            }
            colon_pos = index;
            host_end  = index;
        }
    }

    const size_t host_length = host_end - scheme_length;
    if (host_length == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (colon_pos != authority_end
        && !port_is_valid(input + colon_pos + 1U, authority_end - colon_pos - 1U))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (host_is_ipv4_candidate(input + scheme_length, host_length))
    {
        if (!ipv4_is_valid(input + scheme_length, host_length))
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (!hostname_is_valid(input + scheme_length, host_length))
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t index = 0U; index < authority_end; ++index)
    {
        out_url[index] = index < scheme_length || index < host_end ? ascii_to_lower(input[index]) : input[index];
    }
    out_url[authority_end] = '\0';
    return ESP_OK;
}
