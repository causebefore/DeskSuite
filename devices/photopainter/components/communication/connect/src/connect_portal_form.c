/*
 * 文件职责：实现 Wi-Fi 配网页表单解析的内部纯逻辑。
 * 主要依赖：connect.h。
 * 调用方：Portal HTTP handler。
 */
#include "connect_portal_form.h"

#include <ctype.h>
#include <string.h>

#include "utils.h"

#define CONNECT_PORTAL_ENCODED_VALUE_MAX (CONNECT_SERVICE_URL_MAX * 3)

void connect_portal_url_decode(char *out, size_t out_len, const char *encoded)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (encoded == NULL) {
        out[0] = '\0';
        return;
    }

    size_t oi = 0;
    for (size_t i = 0; encoded[i] != '\0' && oi + 1 < out_len; ++i) {
        if (encoded[i] == '+') {
            out[oi++] = ' ';
        } else if (encoded[i] == '%' && encoded[i + 1] != '\0' && encoded[i + 2] != '\0'
                   && isxdigit((unsigned char) encoded[i + 1])
                   && isxdigit((unsigned char) encoded[i + 2])) {
            const int hi = utils_hex_digit_value(encoded[i + 1]);
            const int lo = utils_hex_digit_value(encoded[i + 2]);
            out[oi++] = (char) ((hi << 4) | lo);
            i += 2;
        } else {
            out[oi++] = encoded[i];
        }
    }
    out[oi] = '\0';
}

static void copy_value(const char *key,
                       const char *value,
                       connect_portal_submission_t *out)
{
    char decoded[CONNECT_SERVICE_URL_MAX] = { 0 };
    connect_portal_url_decode(decoded, sizeof(decoded), value);

    if (strcmp(key, "ssid") == 0) {
        utils_copy_string(out->ssid, sizeof(out->ssid), decoded);
    } else if (strcmp(key, "pass") == 0) {
        utils_copy_string(out->password, sizeof(out->password), decoded);
    } else if (strcmp(key, "service") == 0) {
        utils_copy_string(out->service_url, sizeof(out->service_url), decoded);
    } else if (strcmp(key, "token") == 0) {
        utils_copy_string(out->device_token, sizeof(out->device_token), decoded);
    }
}

bool connect_portal_form_parse(const char *body, connect_portal_submission_t *out)
{
    if (body == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *cursor = body;
    while (*cursor != '\0') {
        char key[16] = { 0 };
        char value[CONNECT_PORTAL_ENCODED_VALUE_MAX] = { 0 };

        size_t key_index = 0;
        while (*cursor != '\0' && *cursor != '=' && *cursor != '&'
               && key_index + 1 < sizeof(key)) {
            key[key_index++] = *cursor++;
        }
        while (*cursor != '\0' && *cursor != '=' && *cursor != '&') {
            cursor++;
        }
        if (*cursor == '=') {
            cursor++;
        }

        size_t value_index = 0;
        while (*cursor != '\0' && *cursor != '&' && value_index + 1 < sizeof(value)) {
            value[value_index++] = *cursor++;
        }
        while (*cursor != '\0' && *cursor != '&') {
            cursor++;
        }
        if (*cursor == '&') {
            cursor++;
        }

        copy_value(key, value, out);
    }

    return out->ssid[0] != '\0';
}
