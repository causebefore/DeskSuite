/**
 * @file web_console_network_provider.h
 * @brief Network Manager 到网页控制台只读状态分区的适配接口
 */
#pragma once

#include "sdkconfig.h"
#include "web_console_provider.h"

#if CONFIG_WEB_CONSOLE_STATUS

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 借用 Network Manager 只读状态 Provider
     *
     * 本接口仅在 `CONFIG_WEB_CONSOLE_STATUS` 开启时声明。
     * 返回的静态描述符和字段元数据在整个固件进程期内有效。Provider 不拥有
     * Network Manager 生命周期；状态回调每次只读取一份最新诊断快照。
     *
     * @return 静态 Status Provider；不会返回 NULL
     */
    const web_console_status_provider_t *web_console_network_provider_get_status_borrow(void);

#ifdef __cplusplus
}
#endif

#endif
