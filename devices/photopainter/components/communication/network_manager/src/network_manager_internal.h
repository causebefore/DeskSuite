/**
 * @file network_manager_internal.h
 * @brief 连接 network_manager 公共门面与内部状态机任务
 */
#pragma once

#include "network_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
 * @brief 借用配置持久化提供者并初始化状态机任务资源
 *
 * @param[in] config_store 配置持久化回调集合
 * @return ESP_OK 已初始化；ESP_ERR_INVALID_ARG 回调集合不完整；或资源错误码
 */
    esp_err_t network_manager_internal_task_init_borrow(
        const network_manager_config_store_t *config_store);

    /** @brief 创建并启动网络管理状态机任务 */
    esp_err_t network_manager_internal_task_start(void);

    /**
 * @brief 同步停止网络管理状态机任务和底层 Wi-Fi 会话
 *
 * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE 生命周期不允许；ESP_ERR_TIMEOUT 尚未收敛；
 *         其他值表示清理失败且生命周期进入不可重启状态
 */
    esp_err_t network_manager_internal_task_stop(void);

    /**
 * @brief 判断 Network Manager 是否仍持有活动或待清理 Task
 *
 * @return true Task 仍存在；false 已完成回收
 */
    bool network_manager_internal_task_has_active_task(void);

    /** @brief 请求状态机保留配置并进入配网 */
    esp_err_t network_manager_internal_request_start_portal(void);

    /** @brief 请求状态机清除配置并进入配网 */
    esp_err_t network_manager_internal_request_forget_and_start_portal(void);

    /**
 * @brief 发布网络状态元数据并在锁外通知回调
 *
 * @param[in] status 待复制状态元数据
 * @param[in] has_saved_config 当前 active 配置是否已经持久化
 */
    void network_manager_internal_publish_status_copy(const network_manager_status_t *status,
                                                      bool has_saved_config);

    /**
 * @brief 更新按需读取的 Portal 展示信息
 *
 * @param[in] info 待复制 Portal 信息
 */
    void network_manager_internal_set_portal_info_copy(const connect_portal_info_t *info);

#ifdef __cplusplus
}
#endif
