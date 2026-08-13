/**
 * @file network_manager.h
 * @brief Wi-Fi 链路、重连与配网状态机对外接口
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "connect.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 网络管理状态 */
    typedef enum
    {
        NETWORK_STATE_STOPPED = 0,
        NETWORK_STATE_CONNECTING,
        NETWORK_STATE_ONLINE,
        NETWORK_STATE_RETRY_WAIT,
        NETWORK_STATE_PROVISIONING,
        NETWORK_STATE_VALIDATING,
        NETWORK_STATE_ERROR,
        NETWORK_STATE_STOPPING,
    } network_manager_state_t;

    /** @brief 网络运行状态元数据 */
    typedef struct
    {
        network_manager_state_t state;                    /**< 当前状态 */
        esp_err_t               last_error;               /**< 当前状态对应的原始错误码 */
        uint32_t                portal_activity_sequence; /**< Portal 显式活动、提交或验证失败序号 */
    } network_manager_status_t;

    /**
 * @brief Network Manager 完整网络诊断快照
 *
 * Manager 状态和计数来自最近一次状态发布；`link` 在读取本快照时向 connect
 * 实时查询。 `link_snapshot_error` 单独报告链路查询结果，不会让本 API
 * 丢失其余诊断事实。所有计数均 以当前 Network Manager
 * 会话为范围，新会话启动时清零。
 */
    typedef struct
    {
        network_manager_status_t status;                 /**< Manager 状态、错误与 Portal 活动序号 */
        connect_link_info_t      link;                   /**< 当前 Wi-Fi、IPv4、网关与 DNS 事实 */
        esp_err_t                link_snapshot_error;    /**< 本次底层链路查询结果 */
        bool                     has_saved_config;       /**< active 配置是否已经持久化 */
        bool                     portal_active;          /**< 配网 Portal 当前是否激活 */
        uint32_t                 session_id;             /**< 当前 Manager 会话编号 */
        uint32_t                 connection_attempts;    /**< STA 连接发起次数，含候选验证 */
        uint32_t                 successful_connections; /**< 到达 ONLINE 的次数 */
        uint32_t                 disconnect_events;      /**< 已处理的断链事件数 */
        uint32_t                 portal_sessions;        /**< Portal 启动成功次数 */
        uint8_t                  current_retry_count;    /**< 当前 active 配置重试序号 */
        uint8_t                  last_disconnect_reason; /**< ESP-IDF 最近断链原始原因码 */
    } network_manager_diagnostics_t;

    /**
 * @brief Network Manager 使用的完整网络配置
 *
 * 所有字符数组都必须在各自容量内以 '\0' 结尾，ssid 不能为空。
 */
    typedef struct
    {
        char ssid[CONNECT_WIFI_SSID_MAX];            /**< Wi-Fi SSID */
        char password[CONNECT_WIFI_PASSWORD_MAX];    /**< Wi-Fi 密码 */
        char service_url[CONNECT_SERVICE_URL_MAX];   /**< 设备服务基础地址 */
        char device_token[CONNECT_DEVICE_TOKEN_MAX]; /**< 设备访问令牌 */
    } network_manager_config_t;

    /**
 * @brief 复制加载已生效网络配置的回调
 *
 * 回调运行于 network_manager_task，可以同步访问有界持久化存储；返回 ESP_OK
 * 前必须完整填写 合法输出，不得保存输出指针。Network Manager
 * 会再次校验字符串边界和必填字段。
 *
 * @param[out] out_config 网络配置输出，仅在返回 ESP_OK 时有效
 * @param[in] ctx 借用的提供者上下文
 * @return ESP_OK 已加载；ESP_ERR_NOT_FOUND 不存在配置；
 *         ESP_ERR_INVALID_SIZE 或 ESP_ERR_INVALID_RESPONSE
 * 表示配置损坏或不兼容；其他值表示存储错误
 */
    typedef esp_err_t (*network_manager_config_load_cb_t)(network_manager_config_t *out_config, void *ctx);

    /**
 * @brief 同步保存完整网络配置的回调
 *
 * 回调运行于
 * network_manager_task，只在调用期间借用配置指针，返回前必须完成持久化。
 *
 * @param[in] config 待保存配置
 * @param[in] ctx 借用的提供者上下文
 * @return ESP_OK 已保存；其他值表示持久化失败
 */
    typedef esp_err_t (*network_manager_config_save_cb_t)(const network_manager_config_t *config, void *ctx);

    /**
 * @brief 清除已生效网络配置的回调
 *
 * 回调运行于 network_manager_task，返回前必须完成持久化清除。
 *
 * @param[in] ctx 借用的提供者上下文
 * @return ESP_OK 已清除；ESP_ERR_NOT_FOUND 原本不存在；其他值表示持久化失败
 */
    typedef esp_err_t (*network_manager_config_erase_cb_t)(void *ctx);

    /** @brief Network Manager 网络配置持久化回调集合 */
    typedef struct
    {
        network_manager_config_load_cb_t  load_config_copy;   /**< 加载配置 */
        network_manager_config_save_cb_t  save_config_borrow; /**< 保存配置 */
        network_manager_config_erase_cb_t erase_config;       /**< 清除配置 */
        void                             *ctx;                /**< 借用的提供者上下文 */
    } network_manager_config_store_t;

    /**
 * @brief 网络状态或 Portal 活动变化通知回调
 *
 * 回调运行于 network_manager_task，必须快速返回且不得阻塞或调用控制接口。
 *
 * @param[in] ctx 用户上下文
 */
    typedef void (*network_manager_notify_cb_t)(void *ctx);

    /**
 * @brief 借用配置持久化回调并初始化网络管理器
 *
 * 函数会复制回调集合，长期保存其中的回调函数指针并借用
 * ctx；调用方必须保证回调实现和 ctx
 * 在固件进程期内始终有效。该函数不启动任务，也不发起 Wi-Fi 连接。
 *
 * @param[in] config_store 配置持久化回调集合
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 回调集合不完整；或其他资源错误码
 */
    esp_err_t network_manager_init_borrow(const network_manager_config_store_t *config_store);

    /**
 * @brief 启动网络管理任务
 *
 * 启动后读取持久化配置；无配置时发布错误事实，由 Application 决定是否请求配网。
 * 每次启动会建立新的内部会话并初始化 Wi-Fi
 * Driver。已运行或正在停止时调用属于非法状态。
 *
 * @return ESP_OK 成功，或其他错误码
 */
    esp_err_t network_manager_start(void);

    /**
 * @brief 同步停止网络会话并释放 Wi-Fi Driver
 *
 * 本函数向 network_manager_task 提交最高优先级停止命令，并在有界时间内等待 Task
 * 完成 Portal、DNS、扫描、Wi-Fi 事件和 Driver 清理后退出。返回 ESP_OK
 * 后可以再次调用
 * network_manager_start()。超时或清理失败时保持不可重启状态，调用方必须进入顶层故障
 * 策略。不得从变化通知回调或 network_manager_task 中调用。
 *
 * @return ESP_OK 已完全停止；ESP_ERR_INVALID_STATE 当前未运行或正在启动；
 *         ESP_ERR_TIMEOUT 停止尚未安全收敛；其他值表示底层清理失败并禁止重启
 */
    esp_err_t network_manager_stop(void);

    /**
 * @brief 复制当前网络状态元数据
 *
 * 状态、错误码和 Portal 活动序号在同一临界区内复制，调用方不会观察到混合状态。
 *
 * @param[out] out_status 状态元数据输出指针
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效
 */
    esp_err_t network_manager_get_status_copy(network_manager_status_t *out_status);

    /**
 * @brief 复制当前完整网络诊断快照
 *
 * 即使 Wi-Fi 会话已停止或底层实时查询失败，本函数仍返回
 * ESP_OK，并把查询错误放入 `link_snapshot_error`；调用方可同时取得最后发布的
 * Manager 状态和会话计数。
 *
 * @param[out] out_diagnostics 完整诊断快照
 * @return ESP_OK 快照已复制；ESP_ERR_INVALID_ARG 输出为空
 */
    esp_err_t network_manager_get_diagnostics_copy(network_manager_diagnostics_t *out_diagnostics);

    /**
 * @brief 复制当前配网 Portal 展示信息
 *
 * 状态为 NETWORK_STATE_PROVISIONING 或 NETWORK_STATE_VALIDATING 且 Portal
 * 已激活时， 返回完整展示信息；其他状态返回 `active=false`
 * 的零值快照。大型二维码载荷只在 Portal 激活且调用本函数时复制。
 *
 * @param[out] out_info Portal 展示信息输出指针
 * @return ESP_OK 快照已复制；ESP_ERR_INVALID_ARG 参数无效
 */
    esp_err_t network_manager_get_portal_info_copy(connect_portal_info_t *out_info);

    /**
 * @brief 查询当前 active 网络配置是否已经持久化
 *
 * @param[out] out_has_saved_config true 表示 active
 * 配置来自持久化存储或已经提交成功
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效
 */
    esp_err_t network_manager_has_saved_config(bool *out_has_saved_config);

    /**
 * @brief 借用网络变化通知回调及其上下文
 *
 * 管理器不接管 callback 或 ctx 的生命周期；再次设置或 network_manager_stop()
 * 完成前，两者必须保持有效，以最先发生者为准。
 *
 * @param[in] callback 回调函数，可为 NULL，表示取消回调
 * @param[in] ctx 借用的用户上下文
 * @return ESP_OK 成功，或其他错误码
 */
    esp_err_t network_manager_set_notify_callback_borrow(network_manager_notify_cb_t callback, void *ctx);

    /**
 * @brief 请求进入配网模式
 *
 * 保留已保存配置，仅停止当前连接并启动 Portal。
 *
 * @return ESP_OK 命令已入队；其他值表示管理器未就绪或队列已满
 */
    esp_err_t network_manager_request_start_portal(void);

#ifdef __cplusplus
}
#endif
