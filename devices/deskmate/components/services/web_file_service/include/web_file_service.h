/**
 * @file web_file_service.h
 * @brief 网页文件服务的公共生命周期与状态快照接口
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
 * @brief 网页文件服务生命周期状态
 */
    typedef enum
    {
        WEB_FILE_SERVICE_STATE_UNINITIALIZED = 0, /**< 尚未创建固定同步资源 */
        WEB_FILE_SERVICE_STATE_INITIALIZED,       /**< 同步资源就绪，未持有 HTTPD */
        WEB_FILE_SERVICE_STATE_STARTING,          /**< 正在创建 HTTPD，尚不接纳请求 */
        WEB_FILE_SERVICE_STATE_RUNNING,           /**< HTTPD 与认证状态就绪，正在接纳请求 */
        WEB_FILE_SERVICE_STATE_STOPPING,          /**< 秘密已失效，正在等待入口、handler 或清理 Task */
        WEB_FILE_SERVICE_STATE_CLEANUP_FAILED,    /**< URI、Task 或 HTTPD 清理失败，可重试停止 */
    } web_file_service_state_t;

    /**
 * @brief 网页文件服务的有界状态快照
 */
    typedef struct
    {
        web_file_service_state_t state;           /**< 当前生命周期状态 */
        bool                     session_active;  /**< 是否存在活动认证会话；不包含会话 token */
        bool                     transfer_active; /**< 是否存在活动文件传输 */
        char                     access_code[7];  /**< NUL 结尾的六位访问码秘密；非运行态为空 */
        esp_err_t                last_error;      /**< 最近一次生命周期错误，成功收敛后为 ESP_OK */
    } web_file_service_status_t;

    /**
 * @brief 初始化网页文件服务的固定同步资源
 *
 * 本同步函数只创建 Service 自有的锁、URI 入口关闭信号量、handler 排空信号量和 HTTPD
 * 清理完成信号量，不访问 SD 卡、不分配文件传输缓冲区，也不启动 HTTPD。只能从普通 Task
 * 上下文调用，且调用方不得让本函数与其他生命周期 API 并发执行。
 *
 * @return ESP_OK 初始化完成；ESP_ERR_INVALID_STATE 已初始化或生命周期状态不允许；
 *         ESP_ERR_NO_MEM 无法创建同步资源
 */
    esp_err_t web_file_service_init(void);

    /**
 * @brief 生成本次运行周期的访问码并启动端口 80 的认证 HTTP 服务
 *
 * 本同步函数仅接受 `INITIALIZED` 状态。HTTPD 及当前 URI handler 全部注册成功后才进入
 * `RUNNING`；启动失败会在固定六秒总期限内回滚到 `INITIALIZED`，超时或清理 HTTPD 失败时
 * 保留一次性清理 Task 与服务器所有权进入 `CLEANUP_FAILED`，并拒绝再次启动，直到后续
 * `stop()` 完成收敛。访问码仅在成功进入 `RUNNING` 后发布到状态快照，并在 `stop()` 开始
 * 时失效；会话 token 仅在访问码校验成功后生成，不由本函数创建或对外暴露。本函数调用
 * HTTPD 外部 API，只能从普通 Task 上下文同步调用。
 *
 * @return ESP_OK 服务已运行；ESP_ERR_INVALID_STATE 当前状态不允许启动；
 *         其他错误码来自随机码格式化、HTTPD 启动、handler 注册或失败回滚
 */
    esp_err_t web_file_service_start(void);

    /**
 * @brief 拒绝新请求、清除秘密并同步停止 HTTP 服务
 *
 * 本函数先原子进入 `STOPPING`、清空访问码和会话 token，再触发关闭全部客户端；随后在
 * `timeout_ms` 的剩余预算内，先等待 HTTPD Task 注销本 Service 的 URI handler 形成稳定
 * 准入屏障，再等待所有已进入的 handler 退出，最后等待一次性清理 Task 完成合法
 * `httpd_stop()`。三阶段共用从函数入口计算的总期限；清理 Task 隔离 ESP-IDF 内部无超时
 * 等待。总期限耗尽时本函数返回 `ESP_ERR_TIMEOUT` 并保留 `STOPPING`，不得强杀仍持有 HTTPD
 * 的 Task，也不释放其句柄或完成信号；后续 `stop()` 会等待同一 Task 并回收结果。工作排队、
 * URI 注销、Task 创建或 HTTPD 清理失败保留 `CLEANUP_FAILED`。两种状态都拒绝 `start()`，
 * 直到后续 `stop()` 成功收敛。本函数只能从普通 Task 上下文调用，不得从已记账的本服务
 * HTTP handler 或清理 Task 内调用。
 *
 * @param[in] timeout_ms 从函数入口计算的三阶段总等待预算，按 FreeRTOS tick 粒度执行
 * @return ESP_OK HTTPD 和运行期资源已经释放并回到 `INITIALIZED`；
 *         ESP_ERR_INVALID_ARG `timeout_ms` 为 0；
 *         ESP_ERR_INVALID_STATE 当前状态不允许停止或已有生命周期操作正在执行；
 *         ESP_ERR_TIMEOUT 入口、handler 或 HTTPD 清理 Task 未在总期限内完成，此时秘密保持
 *         失效且状态为 `STOPPING`；其他错误码来自工作排队、URI 注销、Task 创建或 HTTPD
 *         清理，此时状态为 `CLEANUP_FAILED`
 */
    esp_err_t web_file_service_stop(uint32_t timeout_ms);

    /**
 * @brief 复制网页文件服务的完整有界状态快照
 *
 * 本同步函数只在 Service 状态锁内复制内存，不返回内部指针。未初始化时返回
 * `UNINITIALIZED` 零值快照；成功返回后快照由调用方独立持有，即使 Service 状态随后变化也
 * 不会修改该副本。快照从不包含会话 token；`access_code` 是仅供本地呈现的秘密副本，调用方
 * 不得记录或远程转发，并应在使用完毕后覆盖。函数可能短暂等待状态锁，只能从普通 Task
 * 上下文调用。
 *
 * @param[out] out_status 调用方提供的状态快照输出，成功时完整写入
 * @return ESP_OK 快照有效；ESP_ERR_INVALID_ARG `out_status` 为空
 */
    esp_err_t web_file_service_get_status_copy(web_file_service_status_t *out_status);

    /**
 * @brief 释放网页文件服务的固定同步资源
 *
 * 本同步函数仅接受资源完整收敛后的 `INITIALIZED` 状态；HTTPD、一次性清理 Task 或待收取
 * 结果、已注册 URI、排队中的入口关闭工作、活动 handler、文件传输和传输缓冲区任一仍存在时
 * 均拒绝释放。调用方不得让本函数与状态读取、handler 或其他生命周期 API 并发执行。
 * `STOPPING` 或 `CLEANUP_FAILED` 必须先由 `stop()` 成功收敛，不能通过本函数丢弃仍归
 * Service 所有的 HTTPD 或 Task。本函数只能从普通 Task 上下文调用。
 *
 * @return ESP_OK 已回到 `UNINITIALIZED`；ESP_ERR_INVALID_STATE 状态或资源不满足释放条件
 */
    esp_err_t web_file_service_deinit(void);

#ifdef __cplusplus
}
#endif
