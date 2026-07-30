/**
 * @file power_management_app.h
 * @brief 确认键与刷新周期共同调度整机深睡的 Application
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 电源管理 App 状态 */
    typedef enum
    {
        POWER_MANAGEMENT_APP_STATE_STOPPED = 0, /**< 已停止 */
        POWER_MANAGEMENT_APP_STATE_IDLE, /**< 仅等待手动休眠请求 */
        POWER_MANAGEMENT_APP_STATE_WAIT_REFRESH, /**< 等待本轮内容刷新结束 */
        POWER_MANAGEMENT_APP_STATE_WAIT_DISPLAY, /**< 等待目标集合显示收敛 */
        POWER_MANAGEMENT_APP_STATE_AWAKE_WINDOW, /**< 按键启动后的可操作窗口 */
        POWER_MANAGEMENT_APP_STATE_CHECK_PENDING, /**< 固件检查请求等待内容与显示收敛 */
        POWER_MANAGEMENT_APP_STATE_CHECKING, /**< 正在建立网络并查询固件 */
        POWER_MANAGEMENT_APP_STATE_UPDATE_AVAILABLE, /**< 等待用户确认安装或取消 */
        POWER_MANAGEMENT_APP_STATE_RESULT_PAGE, /**< 显示无更新、服务异常或安装失败结果 */
        POWER_MANAGEMENT_APP_STATE_INSTALLING, /**< 正在执行不可取消固件写入 */
        POWER_MANAGEMENT_APP_STATE_RESTORING, /**< 正在恢复正常缓存照片 */
        POWER_MANAGEMENT_APP_STATE_SLEEP_REQUESTED, /**< 休眠请求已提交 */
        POWER_MANAGEMENT_APP_STATE_RETRY_SLEEP_REQUESTED, /**< 启动失败退避休眠已提交 */
        POWER_MANAGEMENT_APP_STATE_PREPARING_SLEEP, /**< 正在同步停止运行期组件 */
        POWER_MANAGEMENT_APP_STATE_AUTO_SLEEP_BLOCKED, /**< 自动休眠受阻，等待失败退避停机 */
        POWER_MANAGEMENT_APP_STATE_CLEANUP_FAILED, /**< 停机或回滚失败，等待保底深睡 */
    } power_management_app_state_t;

    /** @brief 电源管理 App 初始化配置 */
    typedef struct
    {
        bool automatic_sleep_enabled; /**< 是否在刷新和显示完成后自动深睡 */
        uint32_t interactive_awake_ms; /**< 非定时唤醒后的无活动保持时长 */
    } power_management_app_config_t;

    /** @brief 启动阶段同步深睡策略 */
    typedef enum
    {
        POWER_MANAGEMENT_APP_STARTUP_SLEEP_UNTIL_BUTTON = 0, /**< 仅由按键再次唤醒 */
        POWER_MANAGEMENT_APP_STARTUP_SLEEP_FAILURE_BACKOFF, /**< 按连续失败次数定时重试 */
    } power_management_app_startup_sleep_policy_t;

    /** @brief 电源管理 App 状态快照 */
    typedef struct
    {
        power_management_app_state_t state; /**< 当前状态 */
        bool automatic_sleep_enabled; /**< 是否启用周期性自动深睡 */
        bool timer_wakeup_boot; /**< 本次启动是否由内部定时器唤醒 */
        uint32_t accepted_sleep_requests; /**< 已接收的休眠请求数 */
        uint32_t scheduled_wakeup_seconds; /**< 深睡定时间隔；绝对目标时已含 10 秒防提前补偿 */
        int64_t scheduled_wakeup_at_utc; /**< 服务端绝对唤醒目标 UTC Unix 秒，0 表示相对计划 */
        uint64_t target_collection_generation; /**< 自动休眠等待的集合代数 */
        esp_err_t last_error; /**< 最近一次准备或回滚错误 */
    } power_management_app_status_t;

    /**
 * @brief 初始化电源管理 App 的停止同步资源和自动休眠配置
 *
 * 本函数不创建 Task、不注册回调；调用 start() 前 photo_playback_app 必须已初始化。
 * automatic_sleep_enabled 为 true 时 content_refresh_app 也必须已初始化但尚未启动，确保
 * start() 能在首轮刷新前完成事件订阅。
 *
 * @param[in] config 初始化配置
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 配置无效；ESP_ERR_INVALID_STATE 已初始化；
 *         ESP_ERR_NO_MEM 资源不足；或唤醒来源读取错误码
 */
    esp_err_t power_management_app_init(const power_management_app_config_t *config);

    /**
 * @brief 启动电源管理 Task 并订阅刷新、显示、导航、固件检查和休眠事件
 *
 * @return ESP_OK 已启动；ESP_ERR_INVALID_STATE 生命周期不允许；ESP_ERR_NO_MEM Task 创建失败；
 *         或回调注册错误码
 */
    esp_err_t power_management_app_start(void);

    /**
 * @brief 异步请求进入仅由按键唤醒的手动深睡
 *
 * 请求被接收后，专用 Task 依次停止照片播放、内容刷新、SD 和网络，随后让
 * 墨水屏进入深睡并启动 ESP32-S3 深睡。成功进入深睡时不会产生完成返回，唤醒后从
 * app_main() 重新启动。
 *
 * @return ESP_OK 请求已提交；ESP_ERR_INVALID_STATE 当前不可接收；
 *         ESP_FAIL 通知失败
 */
    esp_err_t power_management_app_request_sleep(void);

    /**
     * @brief 报告内容刷新 Task 启动失败并异步请求退避休眠
     *
     * 仅允许在自动休眠模式已经启动且正在等待首轮刷新时调用。调用方必须先完成失败的
     * content_refresh_app 启动回滚；电源管理 Task 收到事实后会按 RTC 内连续失败次数选择
     * 1/5/15 分钟退避，随后同步停止其余运行期组件并进入定时深睡。
     *
     * @param[in] error 内容刷新 Task 启动错误，不能为 ESP_OK
     * @return ESP_OK 失败事实已提交；ESP_ERR_INVALID_ARG 错误码无效；
     *         ESP_ERR_INVALID_STATE 生命周期或当前状态不允许；ESP_FAIL 通知失败
     */
    esp_err_t power_management_app_report_refresh_start_failure(esp_err_t error);

    /**
     * @brief 在电源管理 Task 尚未初始化时同步收敛启动阶段并进入深睡
     *
     * 本函数供网络准备等早期 Application 失败路径使用。FAILURE_BACKOFF 会更新 RTC 内连续
     * 失败次数并选择 1/5/15 分钟退避；UNTIL_BUTTON 不设置定时器，只保留按键唤醒。
     * 函数会同步停止已经启动的组件、网络和 SD；尚未初始化的显示、网络及其他运行期组件视为
     * 无需清理。显示可用时先让墨水屏进入低功耗，随后启动整机深睡。成功时不返回，只有准备或
     * 回滚失败时才返回错误。调用前本 App 必须尚未初始化。
     *
     * @param[in] policy 启动阶段深睡策略
     * @param[in] reason 触发深睡的原始错误，不能为 ESP_OK
     * @return 仅在未进入深睡时返回：ESP_ERR_INVALID_ARG 参数无效；
     *         ESP_ERR_INVALID_STATE 电源管理 App 已初始化；或停机、唤醒源和显示错误码
     */
    esp_err_t power_management_app_enter_startup_sleep(
        power_management_app_startup_sleep_policy_t policy, esp_err_t reason);

    /**
     * @brief 在运行期组件启动前执行绝对目标时间门禁
     *
     * 本函数应在 PCF8563 已校准 system_clock 后、环境、SD、显示和网络启动前调用，且此时
     * power_management_app 尚未初始化。按键唤醒或非定时器启动会清除旧目标并直接放行；
     * 纯定时器唤醒且可信 UTC 尚未达到保留目标时，会按剩余时间加固定 10 秒防提前补偿重新
     * 进入深睡，成功时不返回。无保留目标或系统时间不可信时降级放行，由后续联网校时恢复。
     *
     * @param[in] woken_by_button 本次是否由任意按键从深睡唤醒
     * @param[in] woken_by_timer 本次是否由内部定时器从深睡唤醒
     * @return ESP_OK 门禁允许继续启动；ESP_ERR_INVALID_STATE 本 App 已初始化或系统时间不可信；
     *         或重新配置深睡唤醒源的底层错误码
     */
    esp_err_t power_management_app_enforce_absolute_wakeup_gate(
        bool woken_by_button, bool woken_by_timer);

    /**
 * @brief 复制电源管理 App 状态
 *
 * @param[out] out_status 状态输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t power_management_app_get_status_copy(power_management_app_status_t *out_status);

    /**
 * @brief 同步停止尚未进入停机事务的电源管理 Task 并解除全部事件回调
 *
 * 仅用于启动回滚或主动关闭运行期；进入深睡的流程不会调用本函数。
 *
 * @return ESP_OK 已停止；ESP_ERR_INVALID_STATE 未运行、正在准备深睡或清理已失败；
 *         ESP_ERR_TIMEOUT Task 未及时退出；或回调清除错误码
 */
    esp_err_t power_management_app_stop(void);

    /**
 * @brief 释放已经停止的电源管理 App 资源
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化、仍在运行或未达到停止终态
 */
    esp_err_t power_management_app_deinit(void);

#ifdef __cplusplus
}
#endif
