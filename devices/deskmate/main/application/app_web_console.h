/**
 * @file app_web_console.h
 * @brief 网页控制台产品流程的异步意图与运行摘要接口
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 网页控制台产品流程状态 */
    typedef enum
    {
        APP_WEB_CONSOLE_STATE_STOPPED = 0,       /**< 未持有运行期 Service 或网络租约 */
        APP_WEB_CONSOLE_STATE_CHECKING_STORAGE,  /**< 正在确认 SD 文件系统与容量 */
        APP_WEB_CONSOLE_STATE_ACQUIRING_NETWORK, /**< 正在申请网页控制台网络租约 */
        APP_WEB_CONSOLE_STATE_STARTING_SERVICE,  /**< 正在初始化并启动网页控制台 Service */
        APP_WEB_CONSOLE_STATE_RUNNING,           /**< 网页控制台 Service 正在运行 */
        APP_WEB_CONSOLE_STATE_STOPPING,          /**< 正在安全停止 Service 并释放网络租约 */
        APP_WEB_CONSOLE_STATE_ERROR,             /**< 启动或停止失败，详情见 last_error */
    } app_web_console_state_t;

    /** @brief 网页控制台的完整有界运行摘要 */
    typedef struct
    {
        app_web_console_state_t state;          /**< 当前产品流程状态 */
        char                    url[32];        /**< 当前 STA IPv4 对应的本地 HTTP URL */
        char                    access_code[7]; /**< 当前 Service 的六位本地访问码 */
        uint64_t                total_bytes;    /**< SD 文件系统总容量，单位字节 */
        uint64_t                free_bytes;     /**< SD 文件系统可用容量，单位字节 */
        esp_err_t               last_error;     /**< 最近一次启动或停止错误 */
    } app_web_console_status_t;

    /**
     * @brief 初始化网页控制台 Application 状态
     *
     * 本同步函数装配并初始化 `web_console_service`，再初始化受保护的产品状态；不启动一次性
     * Task，不访问 SD、网络链路或 HTTPD。Composition Root 必须先初始化 `app_network`。
     * 已处于无保留资源的 `STOPPED` 状态时重复调用保持幂等，支持顶层初始化失败后的安全重试。
     *
     * @return ESP_OK 初始化完成或已安全初始化；ESP_ERR_NO_MEM 无法创建展示推送互斥量；
     *         ESP_ERR_INVALID_STATE 已初始化但产品状态仍活动，或下层 Service 状态不允许接管；
     *         其他值为 Service 运行摘要读取或网络链路回调注册错误；注册失败恢复未初始化状态
     */
    esp_err_t app_web_console_init(void);

    /**
     * @brief 非阻塞请求启动网页控制台
     *
     * 本函数只在短临界区内防止重复启动、切换受保护状态并创建或通知唯一一次性 Application
     * Task；返回时不等待 SD 查询、网络租约、HTTPD 启动或上传事务恢复完成。最终结果通过
     * `app_web_console_get_status_copy()` 读取，并由 Presentation 状态更新事件通知 UI。返回
     * 非 `ESP_OK` 表示命令未被异步接受，调用方必须直接读取当前运行摘要并处理同步错误，不等待
     * 后续 Presentation 事件。
     *
     * @return ESP_OK 启动意图已提交；ESP_ERR_INVALID_STATE 尚未初始化、流程已活动、错误状态
     *         仍持有 Service/租约，或展示版本/停止请求序列已耗尽；ESP_ERR_NO_MEM 无法创建
     *         一次性 Task
     */
    esp_err_t app_web_console_request_start(void);

    /**
     * @brief 非阻塞请求停止网页控制台
     *
     * 本函数只设置受保护的停止请求并通知一次性 Application Task，不等待 HTTPD、handler、
     * 文件传输或网络租约释放。Service 清理失败或超时时保留租约并进入可再次提交停止意图的
     * `ERROR` 状态；只有 HTTPD 已安全停止、Service 已反初始化且租约释放成功后才发布
     * `STOPPED`。返回非 `ESP_OK` 表示本次命令未被异步接受，调用方必须直接读取当前运行摘要并
     * 处理同步错误；若仍保留资源且一次性清理 Task 创建失败，所有权保持不变，后续停止请求
     * 可以重试。
     *
     * @return ESP_OK 停止意图已记录、已停止或清理重试已提交；ESP_ERR_INVALID_STATE 尚未
     *         初始化或停止请求序列已耗尽；ESP_ERR_NO_MEM 无法为保留资源创建一次性清理 Task
     */
    esp_err_t app_web_console_request_stop(void);

    /**
     * @brief 复制网页控制台的完整有界状态
     *
     * 所有字段都在短临界区内整结构复制，不返回内部指针，也不在 Getter 中访问网络、
     * Service 或文件系统。启动 Task 会在 Service 成功后一次性写入真实 URL、六位访问码和
     * `RUNNING`；运行期间由网络 Application 的合并变化通知唤醒同一 Task 更新 URL。
     * 暂时失去关联或 IPv4 时保持 `RUNNING` 但 URL 为空，重连或地址变化后自动发布新运行摘要；
     * 访问码保持不变，且仅供设备本地呈现，调用方不得记录或远程转发。
     *
     * @param[out] out_status 调用方提供的状态输出，成功时完整写入
     * @return ESP_OK 运行摘要有效；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE
     *         Application 尚未初始化
     */
    esp_err_t app_web_console_get_status_copy(app_web_console_status_t *out_status);

#ifdef __cplusplus
}
#endif
