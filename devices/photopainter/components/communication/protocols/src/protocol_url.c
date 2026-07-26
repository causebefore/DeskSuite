/**
 * @file protocol_url.c
 * @brief 设备服务 URL 规范化与拼接实现
 */
#include "protocol_url.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/**
 * @brief 判断基础地址是否含有指定协议前缀
 *
 * @param[in] value 基础地址
 * @param[in] length 基础地址有效长度
 * @param[in] scheme 协议前缀
 * @return true 含有该协议前缀，false 不含有
 */
static bool protocol_url_has_scheme(const char *value, size_t length, const char *scheme)
{
    const size_t scheme_length = strlen(scheme);
    return length >= scheme_length && strncasecmp(value, scheme, scheme_length) == 0;
}

/**
 * @brief 判断有效基础地址中是否含有空白字符
 *
 * @param[in] value 基础地址
 * @param[in] length 基础地址有效长度
 * @return true 含有空白，false 不含有
 */
static bool protocol_url_contains_space(const char *value, size_t length)
{
    for (size_t index = 0; index < length; ++index)
    {
        if (isspace((unsigned char) value[index]) != 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 规范化服务基础地址并拼接 API 路径
 *
 * @param[out] out 完整 URL 输出缓冲区
 * @param[in] out_len 输出缓冲区容量
 * @param[in] base_url 服务基础地址
 * @param[in] path API 相对路径
 * @return ESP_OK 构造成功；ESP_ERR_INVALID_ARG 参数或协议无效；
 *         ESP_ERR_INVALID_SIZE 输出缓冲区不足
 */
esp_err_t protocol_url_build(char *out,
                             size_t out_len,
                             const char *base_url,
                             const char *path)
{
    if (out == NULL || out_len == 0U || base_url == NULL || path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *base_start = base_url;
    while (*base_start != '\0' && isspace((unsigned char) *base_start) != 0)
    {
        base_start++;
    }
    const char *base_end = base_start + strlen(base_start);
    while (base_end > base_start && isspace((unsigned char) base_end[-1]) != 0)
    {
        base_end--;
    }
    while (base_end > base_start && base_end[-1] == '/')
    {
        base_end--;
    }

    const size_t base_length = (size_t) (base_end - base_start);
    const bool has_http = protocol_url_has_scheme(base_start, base_length, "http://");
    const bool has_https = protocol_url_has_scheme(base_start, base_length, "https://");
    const size_t scheme_length = has_http ? strlen("http://")
                                         : (has_https ? strlen("https://") : 0U);
    if (base_length <= scheme_length || protocol_url_contains_space(base_start, base_length)
        || ((!has_http && !has_https) && strstr(base_start, "://") != NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char *path_start = path;
    while (*path_start == '/')
    {
        path_start++;
    }
    const char *default_scheme = has_http || has_https ? "" : "http://";
    const char *separator = path_start[0] == '\0' ? "" : "/";
    const int written = snprintf(out,
                                 out_len,
                                 "%s%.*s%s%s",
                                 default_scheme,
                                 (int) base_length,
                                 base_start,
                                 separator,
                                 path_start);
    return written >= 0 && (size_t) written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
