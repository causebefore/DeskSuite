/*
 * 文件职责：声明 connect 组件多个实现文件之间的资源操作接口。
 * 主要依赖：connect.h。
 * 调用方：connect.c、connect_portal.c 及 Portal 内部任务实现。
 * 仅限 connect 组件 src/ 内部使用（PRIV_INCLUDE_DIRS "src"），不对外暴露。
 */
#ifndef CONNECT_INTERNAL_H
#define CONNECT_INTERNAL_H

#include <stddef.h>

#include "connect.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Portal Wi-Fi 扫描结果 JSON 最大长度 */
#define CONNECT_INTERNAL_PORTAL_SCAN_RESULT_MAX 2048U

    /**
 * @brief 确保 Wi-Fi 驱动已启动
 *
 * @return ESP_OK 已启动或原本已启动；其他值表示驱动启动失败
 */
    esp_err_t connect_internal_wifi_start_once(void);

    /**
 * @brief 确保默认 STA netif 已在 Wi-Fi 启动前创建
 *
 * @return ESP_OK 已存在或创建成功；ESP_ERR_NO_MEM 创建失败
 */
    esp_err_t connect_internal_ensure_sta_netif(void);

    /**
 * @brief 判断 connect 当前是否允许启动新的链路操作
 *
 * @return true 已初始化且未进入反初始化；false 当前拒绝新操作
 */
    bool connect_internal_accepts_operations(void);

    /**
 * @brief 将 Portal 表单提交转交给已注册回调
 *
 * @param[in] submission Portal 表单提交数据
 * @return ESP_OK 调用方已接收；ESP_ERR_INVALID_STATE 未注册回调；其他值由回调返回
 */
    esp_err_t connect_internal_submit_credentials(const connect_portal_submission_t *submission);

    /**
 * @brief 将 Portal 显式用户活动转交给已注册回调
 *
 * @return ESP_OK 调用方已接收；ESP_ERR_INVALID_STATE 未注册回调；其他值由回调返回
 */
    esp_err_t connect_internal_notify_portal_activity(void);

    /**
 * @brief 停止配置 Portal 持有的 HTTP 和扫描资源
 */
    esp_err_t connect_internal_stop_config_portal(void);

    /** @brief 销毁 Portal 持有的默认 AP netif */
    void connect_internal_destroy_ap_netif(void);

    /**
 * @brief 启动 Portal DNS socket 与处理任务
 *
 * @return ESP_OK 已启动或原本已启动；其他值表示 socket、同步资源或任务创建失败
 */
    esp_err_t connect_internal_portal_dns_start(void);

    /**
 * @brief 停止 Portal DNS 处理任务并关闭 socket
 *
 * @return ESP_OK 已停止或原本未启动；ESP_ERR_TIMEOUT 等表示任务停止失败
 */
    esp_err_t connect_internal_portal_dns_stop(void);

    /**
 * @brief 打开并绑定 Portal DNS UDP socket
 *
 * @return ESP_OK 已打开或原本已打开；其他值表示 socket 创建或绑定失败
 */
    esp_err_t connect_internal_portal_dns_open(void);

    /**
 * @brief 阻塞处理一个 Portal DNS 请求
 *
 * @return ESP_OK 已处理或忽略请求；ESP_ERR_INVALID_STATE socket 已关闭；其他值表示收发失败
 */
    esp_err_t connect_internal_portal_dns_process_once(void);

    /** @brief 关闭 Portal DNS socket 并中断正在等待的请求 */
    void connect_internal_portal_dns_close(void);

    /**
 * @brief 请求启动一次 Portal 后台 Wi-Fi 扫描
 *
 * @return ESP_OK 已运行或已启动；其他值表示任务创建失败
 */
    esp_err_t connect_internal_portal_scan_start(void);

    /**
 * @brief 同步停止正在运行的 Portal Wi-Fi 扫描并回收扫描任务
 *
 * @return ESP_OK 已停止或原本未启动；ESP_ERR_TIMEOUT 表示任务未按时退出
 */
    esp_err_t connect_internal_portal_scan_stop(void);

    /** @brief 清空 Portal Wi-Fi 扫描缓存 */
    void connect_internal_portal_scan_reset(void);

    /**
 * @brief 原子复制 Portal Wi-Fi 扫描状态与缓存
 *
 * @param[out] out_cache 扫描结果 JSON 缓冲区
 * @param[in] out_cache_size 扫描结果缓冲区容量
 * @param[out] out_scanning 当前是否正在扫描
 */
    void connect_internal_portal_scan_get(char *out_cache, size_t out_cache_size,
                                          bool *out_scanning);

#ifdef __cplusplus
}
#endif

#endif /* CONNECT_INTERNAL_H */
