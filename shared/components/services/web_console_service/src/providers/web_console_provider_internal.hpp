/**
 * @file web_console_provider_internal.hpp
 * @brief Settings/Status/Actions Provider 注册表与 HTTP 路由私有接口
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "web_console_provider.h"
#include "web_console_provider_validation.hpp"
#include "web_console_service_internal.hpp"

/**
 * @brief 复制并校验初始化时装配的 Settings/Status/Actions Provider 集合
 *
 * 本函数只复制元数据、字符串、枚举值、回调与上下文指针，不调用 Provider。成功后仅
 * Provider `context` 保持长期借用；其他调用方内存可以立即释放。
 *
 * @param[in] settings_providers Settings Provider 数组；数量为零时必须为空
 * @param[in] settings_provider_count Settings Provider 数量
 * @param[in] status_providers Status Provider 数组；数量为零时必须为空
 * @param[in] status_provider_count Status Provider 数量
 * @param[in] action_providers Actions Provider 数组；数量为零时必须为空
 * @param[in] action_provider_count Actions Provider 数量
 * @return ESP_OK 已复制；ESP_ERR_INVALID_ARG 元数据或回调契约无效
 */
esp_err_t web_console_provider_registry_configure_copy(
    const web_console_settings_provider_t *settings_providers,
    size_t settings_provider_count,
    const web_console_status_provider_t *status_providers,
    size_t status_provider_count,
    const web_console_action_provider_t *action_providers,
    size_t action_provider_count);

/**
 * @brief 清空已经复制的 Provider 注册表，不调用或释放借用上下文
 *
 * 只能在没有 Provider HTTP handler 运行时调用；所有 getter 返回的指针随后立即失效。
 */
void web_console_provider_registry_reset(void);

/** @brief 返回已装配 Settings Provider 数量；仅可在配置完成至 reset 前读取。 */
size_t web_console_provider_registry_get_settings_count(void);

/** @brief 按固定装配顺序读取 Settings Provider；指针借用到 reset，索引无效时返回空。 */
const web_console_settings_provider_t *web_console_provider_registry_get_settings(size_t index);

/** @brief 按稳定分区 ID 查找 Settings Provider；指针借用到 reset，不存在时返回空。 */
const web_console_settings_provider_t *web_console_provider_registry_find_settings(const char *section_id);

/** @brief 返回已装配 Status Provider 数量；仅可在配置完成至 reset 前读取。 */
size_t web_console_provider_registry_get_status_count(void);

/** @brief 按固定装配顺序读取 Status Provider；指针借用到 reset，索引无效时返回空。 */
const web_console_status_provider_t *web_console_provider_registry_get_status(size_t index);

/** @brief 按稳定分区 ID 查找 Status Provider；指针借用到 reset，不存在时返回空。 */
const web_console_status_provider_t *web_console_provider_registry_find_status(const char *section_id);

#if CONFIG_WEB_CONSOLE_ACTIONS
/** @brief 返回已装配 Actions Provider 数量；仅可在配置完成至 reset 前读取。 */
size_t web_console_provider_registry_get_action_count(void);

/** @brief 按固定装配顺序读取 Actions Provider；指针借用到 reset，索引无效时返回空。 */
const web_console_action_provider_t *web_console_provider_registry_get_action(size_t index);

/** @brief 按稳定分区 ID 查找 Actions Provider；指针借用到 reset，不存在时返回空。 */
const web_console_action_provider_t *web_console_provider_registry_find_action(const char *section_id);
#endif

/**
 * @brief 返回 Settings/Status/Actions 模块的固定领域路由表
 *
 * @param[out] out_count 路由数量
 * @return 固定路由表；无可选路由时返回空并把数量置零
 */
const web_console_route_t *web_console_provider_get_routes(size_t *out_count);

/** @brief 处理认证后的 Capabilities 读取。 */
esp_err_t web_console_provider_handle_capabilities_get(httpd_req_t *request);
