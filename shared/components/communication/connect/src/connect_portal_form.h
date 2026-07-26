/*
 * 文件职责：声明配网表单的无状态解析函数。
 * 主要依赖：connect.h。
 * 调用方：connect_portal_form.c、Portal HTTP handler。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "connect.h"

#ifdef __cplusplus
extern "C"
{
#endif

    void connect_portal_url_decode(char *out, size_t out_len, const char *encoded);
    bool connect_portal_form_parse(const char *body, connect_portal_submission_t *out);

#ifdef __cplusplus
}
#endif
