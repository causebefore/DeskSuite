/**
 * @file content_refresh_app.h
 * @brief 网络会话、状态上传与显示集合刷新调度 Application
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 内容刷新 App 基础 URL 最大容量 */
#define CONTENT_REFRESH_APP_BASE_URL_MAX 128U
/** @brief 内容刷新 App Token 最大容量 */
#define CONTENT_REFRESH_APP_TOKEN_MAX    96U
/** @brief 内容刷新 App 设备 ID 最大容量 */
#define CONTENT_REFRESH_APP_DEVICE_ID_MAX 81U

/** @brief 内容刷新 App 状态 */
typedef enum
{
    CONTENT_REFRESH_APP_STATE_STOPPED = 0, /**< 已停止 */
    CONTENT_REFRESH_APP_STATE_WAIT_NETWORK, /**< 等待网络可用 */
    CONTENT_REFRESH_APP_STATE_SYNCING,      /**< 执行在线事务 */
    CONTENT_REFRESH_APP_STATE_IDLE,         /**< 正常轮询等待 */
    CONTENT_REFRESH_APP_STATE_BACKOFF,      /**< 错误退避等待 */
    CONTENT_REFRESH_APP_STATE_STOPPING,     /**< 正在清理网络会话 */
    CONTENT_REFRESH_APP_STATE_CLEANUP_FAILED, /**< 网络清理失败 */
} content_refresh_app_state_t;

/** @brief 内容刷新 App 初始化配置 */
typedef struct
{
    const char *base_url;   /**< 服务端基础 URL，init 调用期间借用并复制 */
    const char *token;      /**< 可为空的设备令牌，init 调用期间借用并复制 */
    const char *device_id;  /**< 非空设备 ID，init 调用期间借用并复制 */
    int         timeout_ms; /**< 单次 HTTP 请求超时 */
} content_refresh_app_config_t;

/** @brief 内容刷新 App 状态快照 */
typedef struct
{
    content_refresh_app_state_t state; /**< 当前状态 */
    esp_err_t last_error;              /**< 最近一轮错误 */
    uint8_t consecutive_failures;      /**< 连续同步失败次数 */
    int64_t next_refresh_at_utc;       /**< 服务端下发的下一次刷新 UTC Unix 秒 */
    uint32_t next_retry_ms;            /**< 距下一轮计划等待时长 */
    uint32_t completed_rounds;         /**< 已完成成功轮数 */
} content_refresh_app_status_t;

/** @brief 一轮刷新完成后发布给协调 App 的稳定事实 */
typedef struct
{
    esp_err_t round_error; /**< 本轮联网或同步结果，ESP_OK 表示成功 */
    bool network_cleanup_succeeded; /**< 本轮网络会话是否已经完整关闭 */
    int64_t next_refresh_at_utc; /**< 服务端下发的下一次刷新 UTC Unix 秒 */
    uint64_t collection_generation; /**< 本轮结束时的活动集合代数 */
} content_refresh_app_round_event_t;

/**
 * @brief 内容刷新轮次完成回调
 *
 * 回调在刷新 Task 上下文、App 状态锁外执行，必须快速复制事实并返回，不得同步停止
 * content_refresh_app。
 *
 * @param[in] event 本轮完成事件，仅在回调期间借用
 * @param[in] context 注册时传入的借用上下文
 */
typedef void (*content_refresh_app_round_cb_t)(const content_refresh_app_round_event_t *event,
                                               void *context);

/**
 * @brief 初始化内容刷新 App 并复制长期配置
 *
 * 调用前必须已初始化 system_clock、network_manager、display_collection_service 和环境采样
 * Service。
 *
 * @param[in] config 初始化配置
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 配置无效或超长；ESP_ERR_INVALID_STATE 已初始化；
 *         ESP_ERR_NO_MEM 同步资源创建失败
 */
esp_err_t content_refresh_app_init(const content_refresh_app_config_t *config);

/**
 * @brief 启动刷新 Task，启动后立即执行一轮
 *
 * @return ESP_OK 已启动；ESP_ERR_INVALID_STATE 生命周期不允许；ESP_ERR_NO_MEM Task 创建失败
 */
esp_err_t content_refresh_app_start(void);

/**
 * @brief 非阻塞请求尽快执行一轮手动刷新
 *
 * 多个请求通过 Task Notification 合并。完整刷新正在执行时并入当前轮次；等待期间收到请求时
 * 提前启动新一轮完整刷新。
 *
 * @return ESP_OK 已通知；ESP_ERR_INVALID_STATE Task 未运行或正在停止
 */
esp_err_t content_refresh_app_request_refresh(void);

/**
 * @brief 复制刷新 App 当前状态
 *
 * @param[out] out_status 状态输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t content_refresh_app_get_status_copy(content_refresh_app_status_t *out_status);

/**
 * @brief 设置或清除刷新轮次完成回调
 *
 * 回调借用持续到下一次设置、传入 NULL 清除或 content_refresh_app_deinit()。允许在 Task
 * 运行期间替换，实际调用始终发生在状态锁外。
 *
 * @param[in] callback 回调；NULL 表示清除
 * @param[in] context 回调上下文；callback 为 NULL 时忽略
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t content_refresh_app_set_round_callback_borrow(
    content_refresh_app_round_cb_t callback, void *context);

/**
 * @brief 请求取消当前轮次并同步停止刷新 Task
 *
 * 最长等待“HTTP timeout_ms”与“两次 SNTP 样本超时”中的较大值，再加 10 秒清理余量。
 * HTTP Manifest 请求最多等待自身 timeout_ms，帧下载会在下一个数据块取消。
 *
 * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE 未运行；ESP_ERR_TIMEOUT Task 未退出；
 *         或网络清理错误码
 */
esp_err_t content_refresh_app_stop(void);

/**
 * @brief 释放内容刷新 App 同步资源和配置副本
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化、仍在运行或清理失败
 */
esp_err_t content_refresh_app_deinit(void);

#ifdef __cplusplus
}
#endif
