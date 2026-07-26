/**
 * @file web_file_service_auth.c
 * @brief 网页文件服务的访问码锁定与单会话认证实现
 */
#include "web_file_service_internal.h"

#include <limits.h>
#include <string.h>

/**
 * @brief 有界读取访问码并生成固定六字节比较输入
 *
 * 即使输入提前结束，函数也会填满六字节候选值；格式有效性与后续固定长度比较分离，避免
 * 正确格式访问码按首个不同字符提前结束比较。
 *
 * @param[in] code NUL 结尾的候选访问码
 * @param[out] candidate 固定六字节候选值
 * @return true 恰为六位 ASCII 数字；false 格式无效
 */
static bool web_file_auth_read_code(const char *code, uint8_t candidate[WEB_FILE_ACCESS_CODE_LENGTH])
{
    bool   ended = code == NULL;
    bool   valid = code != NULL;
    size_t offset;

    for (offset = 0U; offset < WEB_FILE_ACCESS_CODE_LENGTH; ++offset)
    {
        unsigned char value = 0U;
        if (!ended)
        {
            value = (unsigned char) *code;
            if (value == '\0')
            {
                ended = true;
            }
            else
            {
                ++code;
            }
        }
        candidate[offset] = value;
        valid             = valid && value >= '0' && value <= '9';
    }
    valid = valid && !ended && *code == '\0';
    return valid;
}

/**
 * @brief 以固定循环次数比较安全敏感字节
 *
 * 差异通过逐字节 XOR 累积，不因首个不同字节提前返回。volatile 累积值阻止编译器把循环
 * 改写为可能提前结束的普通字符串比较。
 *
 * @param[in] left 左侧固定长度字节序列
 * @param[in] right 右侧固定长度字节序列
 * @param[in] size 固定比较长度
 * @return true 完全相等；false 至少一个字节不同
 */
static bool web_file_auth_constant_time_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
    volatile uint8_t difference = 0U;
    for (size_t offset = 0U; offset < size; ++offset)
    {
        difference |= left[offset] ^ right[offset];
    }
    return difference == 0U;
}

static int web_file_auth_hex_value(unsigned char value)
{
    if (value >= '0' && value <= '9')
    {
        return (int) (value - '0');
    }
    if (value >= 'a' && value <= 'f')
    {
        return (int) (value - 'a') + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return (int) (value - 'A') + 10;
    }
    return -1;
}

/**
 * @brief 固定执行十六轮并解码恰好 32 位 token
 *
 * 输入提前结束时不越过 NUL 读取，而是以零补齐剩余候选字节并继续固定轮数。格式结果与
 * 十六字节常量时间比较分离，避免部分 token 参与认证。
 *
 * @param[in] token NUL 结尾的十六进制 token
 * @param[out] decoded 固定十六字节候选 token
 * @return true 恰为 32 位大小写十六进制字符；false 格式无效
 */
static bool web_file_auth_decode_token(const char *token, uint8_t decoded[WEB_FILE_TOKEN_BYTES])
{
    bool ended = token == NULL;
    bool valid = token != NULL;
    bool too_long;

    for (size_t offset = 0U; offset < WEB_FILE_TOKEN_BYTES; ++offset)
    {
        unsigned char high = 0U;
        unsigned char low  = 0U;

        if (!ended)
        {
            high = (unsigned char) *token;
            if (high == '\0')
            {
                ended = true;
            }
            else
            {
                ++token;
            }
        }
        if (!ended)
        {
            low = (unsigned char) *token;
            if (low == '\0')
            {
                ended = true;
            }
            else
            {
                ++token;
            }
        }

        const int high_value = web_file_auth_hex_value(high);
        const int low_value  = web_file_auth_hex_value(low);
        valid                = valid && high_value >= 0 && low_value >= 0;
        decoded[offset]      = (uint8_t) (((high_value < 0 ? 0 : high_value) << 4) | (low_value < 0 ? 0 : low_value));
    }

    too_long = !ended && *token != '\0';
    return valid && !ended && !too_long;
}

/**
 * @brief 判断单会话是否达到空闲失效边界
 *
 * 单调时间倒退表示调用方时序契约已破坏，按失效处理而不是延长会话。达到十分钟边界即失效。
 *
 * @param[in] state 认证状态
 * @param[in] now_us 当前单调时间
 * @return true 会话已失效；false 会话仍在有效期内
 */
static bool web_file_auth_session_is_expired(const web_file_auth_state_t *state, int64_t now_us)
{
    if (!state->session_active)
    {
        return false;
    }
    if (now_us < state->last_activity_us)
    {
        return true;
    }
    return now_us - state->last_activity_us >= WEB_FILE_SESSION_IDLE_TIMEOUT_US;
}

/**
 * @brief 清除当前会话材料但保留访问码和登录锁定状态
 *
 * @param[in,out] state 认证状态
 */
static void web_file_auth_clear_session(web_file_auth_state_t *state)
{
    memset(state->token, 0, sizeof(state->token));
    state->session_active   = false;
    state->last_activity_us = 0;
}

static int64_t web_file_auth_lockout_deadline(int64_t now_us)
{
    if (now_us > INT64_MAX - WEB_FILE_LOGIN_LOCKOUT_US)
    {
        return INT64_MAX;
    }
    return now_us + WEB_FILE_LOGIN_LOCKOUT_US;
}

/**
 * @brief 记录一次访问码失败并在第五次失败时进入锁定
 *
 * @param[in,out] state 认证状态
 * @param[in] now_us 当前单调时间
 * @return WEB_FILE_AUTH_LOCKED 已进入锁定；WEB_FILE_AUTH_BAD_CODE 尚可继续尝试
 */
static web_file_auth_result_t web_file_auth_record_failure(web_file_auth_state_t *state, int64_t now_us)
{
    if (state->failed_attempts < WEB_FILE_LOGIN_MAX_FAILURES)
    {
        ++state->failed_attempts;
    }
    if (state->failed_attempts >= WEB_FILE_LOGIN_MAX_FAILURES)
    {
        state->lockout_until_us = web_file_auth_lockout_deadline(now_us);
        return WEB_FILE_AUTH_LOCKED;
    }
    return WEB_FILE_AUTH_BAD_CODE;
}

void web_file_auth_reset(web_file_auth_state_t *state, const char access_code[7])
{
    if (state == NULL)
    {
        return;
    }

    uint8_t    candidate[WEB_FILE_ACCESS_CODE_LENGTH] = { 0 };
    const bool valid                                  = web_file_auth_read_code(access_code, candidate);
    memset(state, 0, sizeof(*state));
    if (valid)
    {
        memcpy(state->access_code, candidate, sizeof(candidate));
        state->access_code[WEB_FILE_ACCESS_CODE_LENGTH] = '\0';
    }
}

web_file_auth_result_t web_file_auth_create_session(web_file_auth_state_t *state, const char *code,
                                                    const uint8_t random_token[16], int64_t now_us, char out_token[33])
{
    if (state == NULL || random_token == NULL || out_token == NULL || now_us < 0)
    {
        return WEB_FILE_AUTH_UNAUTHORIZED;
    }
    if (state->lockout_until_us > now_us)
    {
        return WEB_FILE_AUTH_LOCKED;
    }
    if (state->lockout_until_us != 0)
    {
        state->lockout_until_us = 0;
        state->failed_attempts  = 0U;
    }

    if (state->session_active)
    {
        if (!web_file_auth_session_is_expired(state, now_us))
        {
            return WEB_FILE_AUTH_SESSION_BUSY;
        }
        web_file_auth_clear_session(state);
    }

    uint8_t    candidate[WEB_FILE_ACCESS_CODE_LENGTH] = { 0 };
    const bool valid_format                           = web_file_auth_read_code(code, candidate);
    const bool code_matches =
        web_file_auth_constant_time_equal(candidate, (const uint8_t *) state->access_code, WEB_FILE_ACCESS_CODE_LENGTH);
    if (!valid_format || !code_matches)
    {
        return web_file_auth_record_failure(state, now_us);
    }

    static const char hex[] = "0123456789abcdef";
    memcpy(state->token, random_token, WEB_FILE_TOKEN_BYTES);
    for (size_t offset = 0U; offset < WEB_FILE_TOKEN_BYTES; ++offset)
    {
        out_token[offset * 2U]      = hex[random_token[offset] >> 4U];
        out_token[offset * 2U + 1U] = hex[random_token[offset] & 0x0FU];
    }
    out_token[WEB_FILE_TOKEN_BUFFER_SIZE - 1U] = '\0';

    state->session_active                      = true;
    state->failed_attempts                     = 0U;
    state->lockout_until_us                    = 0;
    state->last_activity_us                    = now_us;
    return WEB_FILE_AUTH_OK;
}

web_file_auth_result_t web_file_auth_authorize(web_file_auth_state_t *state, const char *bearer, int64_t now_us,
                                               bool transfer_active)
{
    if (state == NULL || now_us < 0)
    {
        return WEB_FILE_AUTH_UNAUTHORIZED;
    }

    uint8_t    candidate[WEB_FILE_TOKEN_BYTES] = { 0 };
    const bool valid_format                    = web_file_auth_decode_token(bearer, candidate);
    if (!state->session_active)
    {
        return WEB_FILE_AUTH_UNAUTHORIZED;
    }
    if (!transfer_active && web_file_auth_session_is_expired(state, now_us))
    {
        web_file_auth_clear_session(state);
        return WEB_FILE_AUTH_EXPIRED;
    }

    const bool token_matches = web_file_auth_constant_time_equal(candidate, state->token, WEB_FILE_TOKEN_BYTES);
    if (!valid_format || !token_matches)
    {
        return WEB_FILE_AUTH_UNAUTHORIZED;
    }

    if (now_us > state->last_activity_us)
    {
        state->last_activity_us = now_us;
    }
    return WEB_FILE_AUTH_OK;
}

void web_file_auth_touch_active_session(web_file_auth_state_t *state, int64_t now_us)
{
    if (state != NULL && state->session_active && now_us >= 0 && now_us > state->last_activity_us)
    {
        state->last_activity_us = now_us;
    }
}
