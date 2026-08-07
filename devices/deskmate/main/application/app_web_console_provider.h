/**
 * @file app_web_console_provider.h
 * @brief DeskMate 网页控制台产品 Provider 装配接口
 */
#pragma once

#include <stddef.h>

#include "web_console_provider.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 借用本次构建启用的 DeskMate Settings Provider 数组
     *
     * 返回的静态数组覆盖整个固件进程；关闭 Settings 构建开关时返回 NULL 且数量为 0。
     *
     * @param[out] out_count Provider 数量，不能为空
     * @return 静态 Provider 数组；未启用或参数为空时为 NULL
     */
    const web_console_settings_provider_t *app_web_console_provider_get_settings_borrow(size_t *out_count);

    /**
     * @brief 借用本次构建启用的 DeskMate Status Provider 数组
     *
     * 返回的静态数组覆盖整个固件进程；关闭 Status 构建开关时返回 NULL 且数量为 0。
     *
     * @param[out] out_count Provider 数量，不能为空
     * @return 静态 Provider 数组；未启用或参数为空时为 NULL
     */
    const web_console_status_provider_t *app_web_console_provider_get_status_borrow(size_t *out_count);

    /**
     * @brief 借用本次构建启用的 DeskMate Actions Provider 数组
     *
     * 返回的静态数组覆盖整个固件进程；关闭 Actions 构建开关时返回 NULL 且数量为 0。
     *
     * @param[out] out_count Provider 数量，不能为空
     * @return 静态 Provider 数组；未启用或参数为空时为 NULL
     */
    const web_console_action_provider_t *app_web_console_provider_get_actions_borrow(size_t *out_count);

#ifdef __cplusplus
}
#endif
