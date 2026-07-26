/**
 * @file display_collection_service.h
 * @brief 多页面显示集合的同步、持久化与原子切换 Service
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_protocol.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief SD 相对文件路径容量 */
#define DISPLAY_COLLECTION_PATH_MAX 192U

/** @brief 一个可显示页面的本地副本描述 */
typedef struct
{
    display_protocol_page_t protocol;                         /**< 服务端协议元数据 */
    char                    file_path[DISPLAY_COLLECTION_PATH_MAX]; /**< SD 相对文件路径 */
} display_collection_page_t;

/** @brief 集合 Service 对外发布的不可变状态快照 */
typedef struct
{
    bool      storage_available; /**< SD 文件系统当前可用且恢复完成 */
    bool      has_active;        /**< 存在可读取的活动集合 */
    bool      syncing;           /**< 正在执行同步事务 */
    uint64_t  generation;        /**< 最近成功提交的本地状态代数 */
    char      active_collection[DISPLAY_PROTOCOL_VERSION_MAX];   /**< 活动集合版本 */
    char      previous_collection[DISPLAY_PROTOCOL_VERSION_MAX]; /**< 上一集合版本 */
    char      default_page[DISPLAY_PROTOCOL_PAGE_ID_MAX];        /**< 默认页面 ID */
    uint8_t   page_count;                                    /**< 活动页面数量 */
    int64_t   next_refresh_at_utc;                            /**< 下一次刷新 UTC Unix 秒 */
    esp_err_t last_error;                                    /**< 最近一次恢复或同步错误 */
} display_collection_snapshot_t;

/** @brief 同步事务提交结果 */
typedef enum
{
    DISPLAY_COLLECTION_SYNC_NOT_MODIFIED = 0, /**< 服务端返回 304 或版本未变化 */
    DISPLAY_COLLECTION_SYNC_COMMITTED,        /**< 新集合已完整提交 */
} display_collection_sync_outcome_t;

/** @brief 同步事务结果 */
typedef struct
{
    display_collection_sync_outcome_t outcome;          /**< 事务结果 */
    uint8_t                           downloaded_pages; /**< 实际下载页面数 */
    uint8_t                           reused_pages;     /**< 复用本地页面数 */
    int64_t                           next_refresh_at_utc; /**< 下一次刷新 UTC Unix 秒 */
} display_collection_sync_result_t;

/**
 * @brief 查询同步事务是否应取消
 *
 * @param[in] context 注册时传入的借用上下文
 * @return true 请求取消；false 继续执行
 */
typedef bool (*display_collection_cancel_cb_t)(void *context);

/** @brief 一次同步事务配置，仅在同步调用期间借用字符串与回调 */
typedef struct
{
    const char                     *base_url;    /**< 服务端基础 URL */
    const char                     *token;       /**< 可为空的设备令牌 */
    const char                     *device_id;   /**< 设备 ID */
    int                             timeout_ms;  /**< 单次 HTTP 请求超时 */
    display_collection_cancel_cb_t  should_cancel; /**< 可为空的取消查询 */
    void                           *cancel_context; /**< 取消查询上下文 */
} display_collection_sync_request_t;

/**
 * @brief 活动集合提交回调
 *
 * 回调在发起同步的调用方上下文中、Service 内部锁之外执行。snapshot 只在回调期间借用，
 * 回调不得重入同步或生命周期 API；耗时业务应快速复制事实并投递给自己的 Task。
 *
 * @param[in] snapshot 新活动集合快照
 * @param[in] context 注册时传入的借用上下文
 */
typedef void (*display_collection_commit_cb_t)(const display_collection_snapshot_t *snapshot,
                                               void *context);

/**
 * @brief 初始化集合 Service 并尝试从 SD 恢复活动集合
 *
 * 调用前必须已初始化 device_sd 并启动 sd_card_service。SD 尚不可用或持久化内容损坏时，
 * Service 仍完成初始化，并通过快照的 storage_available、has_active 和 last_error 报告事实。
 *
 * @return ESP_OK 初始化完成；ESP_ERR_INVALID_STATE 已初始化；ESP_ERR_NO_MEM 资源不足
 */
esp_err_t display_collection_service_init(void);

/**
 * @brief 同步执行一次完整集合刷新事务
 *
 * 事务期间独占内部下载缓冲区，但快照读取仍可并发执行。任何页面失败都不会切换活动集合。
 *
 * @param[in] request 同步配置，仅调用期间借用
 * @param[out] out_result 同步结果，仅 ESP_OK 时有效
 * @return ESP_OK 未变化或已提交；ESP_ERR_INVALID_STATE 生命周期、并发或存储状态无效；
 *         ESP_ERR_INVALID_ARG 参数无效；或网络、协议、存储错误码
 */
esp_err_t display_collection_service_sync(const display_collection_sync_request_t *request,
                                          display_collection_sync_result_t *out_result);

/**
 * @brief 复制当前集合状态快照
 *
 * @param[out] out_snapshot 快照输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t display_collection_service_get_snapshot_copy(
    display_collection_snapshot_t *out_snapshot);

/**
 * @brief 按活动集合索引复制页面描述
 *
 * @param[in] index 页面索引
 * @param[out] out_page 页面输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或索引无效；ESP_ERR_INVALID_STATE 无活动集合
 */
esp_err_t display_collection_service_get_page_copy(uint8_t index,
                                                   display_collection_page_t *out_page);

/**
 * @brief 按稳定页面 ID 复制活动页面描述
 *
 * @param[in] page_id 页面 ID
 * @param[out] out_page 页面输出
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 页面不存在；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 无活动集合
 */
esp_err_t display_collection_service_find_page_copy(const char *page_id,
                                                    display_collection_page_t *out_page);

/**
 * @brief 设置或清除活动集合提交回调
 *
 * 回调与 context 的借用持续到下一次设置、传入 NULL 清除或 Service 反初始化。
 *
 * @param[in] callback 提交回调；NULL 表示清除
 * @param[in] context 借用上下文
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t display_collection_service_set_commit_callback_borrow(
    display_collection_commit_cb_t callback, void *context);

/**
 * @brief 释放集合 Service 的锁和 PSRAM 缓冲区
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或同步事务仍在运行
 */
esp_err_t display_collection_service_deinit(void);

#ifdef __cplusplus
}
#endif
