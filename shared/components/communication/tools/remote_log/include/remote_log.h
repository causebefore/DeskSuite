/**
 * @file remote_log.h
 * @brief ESP-IDF 远端日志采集与上传工具
 * @note 除状态快照外，初始化、配置、启动、停止和反初始化接口必须由编排方串行调用
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 服务端日志会话 ID 缓冲区容量（含结尾空字符） */
#define REMOTE_LOG_SESSION_ID_MAX 96U

    /** @brief 远端日志 Task 状态 */
    typedef enum
    {
        REMOTE_LOG_STATE_STOPPED = 0,      /**< Task 未运行，Log V2 捕获仍可缓存日志 */
        REMOTE_LOG_STATE_WAITING_NETWORK,  /**< 等待 Network Manager 进入 ONLINE */
        REMOTE_LOG_STATE_STARTING_SESSION, /**< 正在创建服务端日志会话 */
        REMOTE_LOG_STATE_IDLE,             /**< 会话已建立，等待日志 */
        REMOTE_LOG_STATE_UPLOADING,        /**< 正在上传或重试当前日志批次 */
        REMOTE_LOG_STATE_STOPPING,         /**< 已请求停止，等待当前同步协议调用返回 */
    } remote_log_state_t;

    /** @brief 远端日志资源与重试配置 */
    typedef struct
    {
        size_t   queue_capacity;        /**< 可缓存的日志条数，不含内部停止命令槽位 */
        size_t   batch_capacity;        /**< 单次上传的最大日志条数，不得大于 queue_capacity */
        uint32_t batch_wait_ms;         /**< 收到首条日志后继续聚合批次的等待时间 */
        uint32_t retry_interval_ms;     /**< 离线或上传失败后的重试间隔 */
        int      http_timeout_ms;       /**< log_upload 同步 HTTP 请求超时 */
        uint32_t task_stack_size_bytes; /**< ESP-IDF FreeRTOS Task 栈字节数 */
        uint32_t task_priority;         /**< FreeRTOS Task 优先级 */
    } remote_log_config_t;

    /** @brief 远端日志运行状态快照 */
    typedef struct
    {
        remote_log_state_t state;                                 /**< 当前生命周期和上传状态 */
        uint32_t           captured_lines;                        /**< 已成功进入内部队列的日志数 */
        uint32_t           queued_lines;                          /**< 当前仍在内部队列等待处理的日志数 */
        uint32_t           uploaded_lines;                        /**< 已由服务端成功接受的日志数 */
        uint32_t           dropped_lines;                         /**< 因队列满或停止时放弃的日志数 */
        uint32_t           upload_failures;                       /**< 会话创建与批次上传失败次数 */
        esp_err_t          last_error;                            /**< 最近一次网络状态或上传错误；成功后恢复 ESP_OK */
        char               session_id[REMOTE_LOG_SESSION_ID_MAX]; /**< 当前服务端会话；尚未建立时为空 */
    } remote_log_status_t;

    /**
 * @brief 写入推荐的默认资源与重试配置
 *
 * 默认队列 64 条、每批 8 条、聚合等待 100 ms、失败重试 3 s、HTTP 超时 3 s、
 * Task 栈 6144 字节、优先级 3。
 *
 * @param[out] out_config 配置输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出指针为空
 */
    esp_err_t remote_log_config_set_defaults(remote_log_config_t *out_config);

    /**
 * @brief 初始化日志缓存资源并启用 ESP-IDF Log V2 捕获
 *
 * 初始化后启用已经通过链接器接入的 Log V2 捕获，立即开始非阻塞缓存普通 ESP_LOGx 输出，
 * 但不创建上传 Task。链接器包装继续调用 esp_log_va()，因此原串口输出保持不变。队列满时
 * 拒绝最新日志并累计 dropped_lines。本组件不设置或独占 esp_log_set_vprintf()。
 *
 * @param[in] config 资源与重试配置，仅在调用期间借用并完整复制
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 配置无效；ESP_ERR_INVALID_STATE 已初始化；
 *         ESP_ERR_NO_MEM 资源创建失败
 */
    esp_err_t remote_log_init(const remote_log_config_t *config);

    /**
 * @brief 复制服务端地址、产品 ID 与设备 ID
 *
 * 仅允许在上传 Task 停止时调用。修改配置会清空已有 session_id，但不会清空已缓存日志。
 * 本组件直接使用 protocols/log_upload，不读取 Storage，也不保存或使用设备 Token。
 *
 * @param[in] base_url 非空服务端基础 URL
 * @param[in] product_id 大于 0 的产品标识
 * @param[in] device_id 非空设备 ID
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 产品 ID 为 0、字符串为空或过长；
 *         ESP_ERR_INVALID_STATE 尚未初始化或 Task 未停止
 */
    esp_err_t remote_log_configure_copy(const char *base_url, uint32_t product_id, const char *device_id);

    /**
 * @brief 启动独立上传 Task
 *
 * 调用前必须完成 network_manager_init_borrow() 和 remote_log_configure_copy()。本函数不启动或停止
 * Network Manager；Task 会读取其状态，仅在 NETWORK_STATE_ONLINE 时同步调用 log_upload。
 *
 * @return ESP_OK Task 已创建；ESP_ERR_INVALID_STATE 生命周期或配置不允许；
 *         ESP_ERR_NO_MEM Task 创建失败
 */
    esp_err_t remote_log_start(void);

    /**
 * @brief 同步停止上传 Task
 *
 * 停止请求不会取消正在执行的同步 log_upload 调用；函数等待其返回并让 Task 到达安全停止点。
 * 尚未上传的当前内存批次会计入 dropped_lines，队列中未取出的日志保留供后续 start 使用。
 * 超时后保持 REMOTE_LOG_STATE_STOPPING，可再次调用本函数继续等待。
 *
 * @param[in] timeout_ms 等待 Task 到达停止点的最长时间
 * @return ESP_OK 已停止；ESP_ERR_INVALID_ARG timeout_ms 为 0；
 *         ESP_ERR_INVALID_STATE Task 未运行；ESP_ERR_TIMEOUT 尚未停止
 */
    esp_err_t remote_log_stop(uint32_t timeout_ms);

    /**
 * @brief 停用 Log V2 捕获并释放全部资源
 *
 * 调用方必须保证上传 Task 已停止。函数先停用捕获并等待已经进入捕获流程的调用退出，再释放
 * 缓存资源；返回后本组件不再捕获日志，缓存中尚未处理的数据被释放。链接器包装仍继续调用
 * esp_log_va()，因此停用捕获不会影响原串口输出。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或 Task 未完全停止
 */
    esp_err_t remote_log_deinit(void);

    /**
 * @brief 原子复制当前远端日志状态
 *
 * @param[out] out_status 状态输出，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出指针为空；
 *         ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t remote_log_get_status_copy(remote_log_status_t *out_status);

#ifdef __cplusplus
}
#endif
