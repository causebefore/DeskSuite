/**
 * @file firmware_ota.h
 * @brief 独立应用固件 OTA 通信工具
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FIRMWARE_OTA_VERSION_MAX      64U
#define FIRMWARE_OTA_ARTIFACT_ID_SIZE 65U

    /** @brief OTA Task 状态 */
    typedef enum
    {
        FIRMWARE_OTA_STATE_STOPPED = 0,      /**< Task 未运行 */
        FIRMWARE_OTA_STATE_IDLE,             /**< 等待检查命令 */
        FIRMWARE_OTA_STATE_CHECKING,         /**< 正在检查清单 */
        FIRMWARE_OTA_STATE_UPDATE_AVAILABLE, /**< 已缓存待用户确认的不可变目标 */
        FIRMWARE_OTA_STATE_DOWNLOADING,      /**< 正在下载并写入备用分区 */
        FIRMWARE_OTA_STATE_AWAITING_RESTART, /**< 已切换启动分区，等待强制重启 */
    } firmware_ota_state_t;

    /** @brief OTA 工具生命周期配置 */
    typedef struct
    {
        int check_timeout_ms;    /**< 检查接口 HTTP 超时 */
        int download_timeout_ms; /**< 固件下载 HTTP 超时 */
    } firmware_ota_config_t;

    /** @brief 单次在线检查结果 */
    typedef struct
    {
        bool     update_available;                                  /**< 是否已缓存可供确认安装的新制品 */
        char     target_version[FIRMWARE_OTA_VERSION_MAX];          /**< 目标诊断版本 */
        uint64_t target_ota_version;                                /**< 目标单调 OTA 版本 */
        char     target_artifact_id[FIRMWARE_OTA_ARTIFACT_ID_SIZE]; /**< 目标不可变标识 */
        size_t   target_size;                                       /**< 目标固件字节数 */
    } firmware_ota_check_result_t;

    /** @brief OTA 异步事务完成事件类型 */
    typedef enum
    {
        FIRMWARE_OTA_EVENT_CHECK_COMPLETED = 0, /**< 检查事务已经完成 */
        FIRMWARE_OTA_EVENT_INSTALL_COMPLETED,   /**< 安装事务已经完成；成功时设备将立即重启 */
    } firmware_ota_event_type_t;

    /**
 * @brief OTA 异步事务完成事件
 *
 * `result` 是事务最终结果，`state` 是事件产生时的稳定状态。仅
 * `FIRMWARE_OTA_EVENT_CHECK_COMPLETED` 且 `result == ESP_OK` 时 `check_result` 有效。
 */
    typedef struct
    {
        firmware_ota_event_type_t   type;         /**< 完成事件类型 */
        esp_err_t                   result;       /**< 事务最终结果 */
        firmware_ota_state_t        state;        /**< 事务完成后的状态 */
        firmware_ota_check_result_t check_result; /**< 检查结果副本 */
    } firmware_ota_event_t;

    /**
 * @brief OTA 异步事务完成回调
 *
 * 回调在 OTA Task 上下文、组件内部锁之外执行，必须快速返回，不得重入 OTA 控制 API。
 * 命令入队后 OTA Task 可能立即抢占运行，因此回调不保证晚于 `request` API 返回。`event`
 * 仅在本次回调期间有效，调用方需要跨 Task 使用时必须按值复制。
 *
 * @param[in] event 异步事务完成事件
 * @param[in] context 调用方上下文
 */
    typedef void (*firmware_ota_event_cb_t)(const firmware_ota_event_t *event, void *context);

    /** @brief 当前固件身份快照 */
    typedef struct
    {
        char     current_version[FIRMWARE_OTA_VERSION_MAX];               /**< 当前应用诊断版本 */
        uint64_t current_ota_version;                                     /**< 当前固件构建时嵌入的单调 OTA 版本 */
        char     current_artifact_id[FIRMWARE_OTA_ARTIFACT_ID_SIZE];      /**< 当前镜像 Validation SHA-256 */
        bool     has_last_invalid_artifact;                               /**< 是否存在上次无效镜像 */
        char     last_invalid_artifact_id[FIRMWARE_OTA_ARTIFACT_ID_SIZE]; /**< 上次无效镜像标识 */
    } firmware_ota_identity_t;

    /**
 * @brief 初始化 OTA 运行资源，但不启动 Task 或启停 Wi-Fi
 *
 * @param[in] config 生命周期配置，仅在调用期间借用
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 已初始化；
 *         ESP_ERR_NO_MEM 运行资源创建失败
 */
    esp_err_t firmware_ota_init(const firmware_ota_config_t *config);

    /**
 * @brief 设置或清除异步事务完成回调
 *
 * 仅允许在 `STOPPED`、`IDLE` 或 `UPDATE_AVAILABLE` 状态调用。组件长期借用回调与上下文，
 * 直到后续调用替换、传入 `NULL` 清除或 `deinit()` 成功。提交检查或安装前必须已经注册回调。
 *
 * @param[in] callback 完成回调；`NULL` 表示清除
 * @param[in] context 回调上下文；callback 为 `NULL` 时必须同时为 `NULL`
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数组合无效；
 *         ESP_ERR_INVALID_STATE 生命周期或事务状态不允许
 */
    esp_err_t firmware_ota_set_event_callback_borrow(firmware_ota_event_cb_t callback, void *context);

    /**
 * @brief 复制当前服务端连接配置
 *
 * 仅允许在 Task 停止或空闲时调用。工具不会启动、停止或等待 Wi-Fi。
 *
 * @param[in] base_url 服务端基础地址
 * @param[in] token 可为空的共享设备 Token
 * @param[in] product_id 大于 0 的产品标识
 * @param[in] firmware_target 小写字母开头、仅含小写字母/数字/下划线的固件兼容目标
 * @param[in] device_id 非空设备 ID
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 字符串无效或过长；
 *         ESP_ERR_INVALID_STATE 生命周期或状态不允许
 */
    esp_err_t firmware_ota_configure_copy(const char *base_url,
                                          const char *token,
                                          uint32_t    product_id,
                                          const char *firmware_target,
                                          const char *device_id);

    /**
 * @brief 启动独立 OTA Task
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 生命周期不允许；ESP_ERR_NO_MEM 创建失败
 */
    esp_err_t firmware_ota_start(void);

    /**
 * @brief 异步提交一次只查询不下载的 OTA 检查
 *
 * 调用方必须先确认网络在线。本函数返回时只表示命令已经提交；完成结果通过注册回调返回。
 * 组件同一时刻只接受一个事务，队列满时拒绝本次提交。发现更新时，完整不可变目标缓存在
 * OTA Runtime，状态进入 `UPDATE_AVAILABLE`，后续只能安装或丢弃该目标。
 *
 * @return ESP_OK 命令已提交；ESP_ERR_INVALID_STATE 生命周期、配置、回调或状态不允许；
 *         ESP_ERR_TIMEOUT 命令队列已满
 */
    esp_err_t firmware_ota_request_check(void);

    /**
 * @brief 异步提交已经缓存的待确认固件安装
 *
 * 仅接受 `UPDATE_AVAILABLE` 状态。本函数返回时只表示命令已经提交；命令一旦提交即不可取消。
 * 完成结果通过注册回调返回。成功切换启动分区后组件立即调用 `esp_restart()`；下载、校验或
 * 写入失败会清除待安装目标并回到 `IDLE`，调用方必须重新检查后才能重试。
 *
 * @return ESP_OK 命令已提交；ESP_ERR_INVALID_STATE 没有待安装目标或未注册回调；
 *         ESP_ERR_TIMEOUT 命令队列已满
 */
    esp_err_t firmware_ota_request_install(void);

    /**
 * @brief 丢弃已缓存但尚未开始安装的固件目标
 *
 * UPDATE_AVAILABLE 状态下清除目标并回到 IDLE；IDLE 状态幂等成功。检查、下载或等待重启
 * 阶段拒绝调用。
 *
 * @return ESP_OK 已清除或原本无目标；ESP_ERR_INVALID_STATE 生命周期或状态不允许
 */
    esp_err_t firmware_ota_discard_pending_update(void);

    /**
 * @brief 同步停止 OTA Task
 *
 * 提交停止命令后立即拒绝新的控制请求。若检查或下载已开始，本函数等待事务完成及其完成回调
 * 返回后再停止，不发送取消信号；停止时清除待确认目标。
 *
 * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE 未运行或已等待强制重启；
 *         ESP_ERR_TIMEOUT 未在期限内退出
 */
    esp_err_t firmware_ota_stop(void);

    /**
 * @brief 释放 OTA 同步资源
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或 Task 仍运行
 */
    esp_err_t firmware_ota_deinit(void);

    /**
 * @brief 复制 OTA Task 当前状态
 * @param[out] out_state 状态输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t firmware_ota_get_state_copy(firmware_ota_state_t *out_state);

    /**
 * @brief 复制当前运行镜像和上次无效镜像身份
 * @param[out] out_identity 身份输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或分区摘要读取错误码
 */
    esp_err_t firmware_ota_get_identity_copy(firmware_ota_identity_t *out_identity);

    /**
 * @brief 若当前镜像待验证，则确认本次本地健康启动
 * @return ESP_OK 已确认或无需确认；其他值表示读取或写入 OTA 状态失败
 */
    esp_err_t firmware_ota_confirm_running_image(void);

    /**
 * @brief 若当前镜像待验证，则标记无效并立即回滚重启
 *
 * @return ESP_OK 当前镜像无需回滚；其他值表示状态读取失败。成功回滚时不返回。
 */
    esp_err_t firmware_ota_reject_running_image_and_reboot(void);

#ifdef __cplusplus
}
#endif
