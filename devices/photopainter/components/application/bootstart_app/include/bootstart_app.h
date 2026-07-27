/**
 * @file bootstart_app.h
 * @brief PhotoPainter 启动阶段用例接口
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 一轮启动读取到的深睡唤醒上下文 */
    typedef struct
    {
        bool      woken_by_button; /**< 是否由任意按键从深睡唤醒 */
        bool      woken_by_timer;  /**< 是否由内部定时器从深睡唤醒 */
        esp_err_t read_error;      /**< 读取唤醒来源的结果 */
    } bootstart_app_wakeup_context_t;

    /**
 * @brief 复制读取本轮深睡唤醒上下文
 *
 * 底层读取失败仍返回 ESP_OK，并通过 `out_context->read_error` 保留错误事实，供顶层启动编排
 * 决定是否继续。函数同步完成，不保存输出指针。
 *
 * @param[out] out_context 唤醒上下文输出，仅在返回 ESP_OK 时有效
 * @return ESP_OK 已复制上下文；ESP_ERR_INVALID_ARG 输出指针为空
 */
    esp_err_t bootstart_app_get_wakeup_context_copy(bootstart_app_wakeup_context_t *out_context);

    /**
 * @brief 初始化系统持久化和可信系统时钟基础资源
 *
 * @return ESP_OK 成功；或存储、系统时钟初始化错误码
 */
    esp_err_t bootstart_app_init_system_services(void);

    /**
 * @brief 初始化 RTC、必要时写入编译时间并校准系统墙上时钟
 *
 * @return ESP_OK RTC 已可用；或 RTC 初始化、读取、写入错误码
 */
    esp_err_t bootstart_app_init_rtc(void);

    /**
 * @brief 记录唤醒来源并执行启动绝对目标时间门禁
 *
 * @param[in] context 调用期间借用的本轮唤醒上下文
 * @return ESP_OK 门禁允许继续；ESP_ERR_INVALID_ARG 参数为空；或门禁错误码
 */
    esp_err_t
        bootstart_app_enforce_absolute_wakeup_gate(const bootstart_app_wakeup_context_t *context);

    /**
 * @brief 初始化温湿度、电池和按需环境采样能力
 *
 * @return ESP_OK 成功；或首个初始化错误码
 */
    esp_err_t bootstart_app_start_environment(void);

    /**
 * @brief 初始化 SD Device 并启动插拔自动挂载 Service
 *
 * @return ESP_OK 成功；或首个初始化、启动错误码
 */
    esp_err_t bootstart_app_start_sd(void);

    /**
 * @brief 初始化四灰阶显示、网络管理器、远端日志捕获和固件 OTA Task
 *
 * 成功返回前会完成待验证 OTA 镜像的本地健康确认。
 *
 * @return ESP_OK 本地关键通信能力已就绪；或初始化、启动错误码
 */
    esp_err_t bootstart_app_init_local_communication(void);

    /**
 * @brief 使用已初始化的显示与网络能力运行配网用例，并在联网后尽力启动远端日志上传
 *
 * @param[in] wakeup_context 调用期间借用的本轮唤醒上下文
 * @return ESP_OK 网络在线；ESP_ERR_INVALID_ARG 参数为空；或配网错误码
 */
    esp_err_t bootstart_app_run_provisioning(const bootstart_app_wakeup_context_t *wakeup_context);

    /**
 * @brief 初始化本地集合、页面呈现、按键播放和内容刷新链路
 *
 * 本函数会启动照片播放 App，但只初始化内容刷新 App；首轮内容刷新必须等待电源管理完成回调
 * 订阅后再由 `bootstart_app_start_content_refresh()` 启动。统一后端上下文或内容刷新初始化失败
 * 均作为本轮致命启动错误返回，不保留缺少网络收敛和休眠所有者的半运行模式。
 * 照片播放启动后返回错误时，调用方必须立即调用 `bootstart_app_handle_fatal_error()`；函数会
 * 暂时保留已运行组件，由统一停机事务按依赖顺序收敛，调用方不得继续启动或自行交错清理。
 *
 * @return ESP_OK 照片播放已启动且内容刷新已初始化；或显示、后端上下文和内容刷新错误码
 */
    esp_err_t bootstart_app_start_photo_pipeline(void);

    /**
 * @brief 尽力初始化 LED 与蜂鸣器反馈能力
 *
 * 失败仅记录诊断，不阻断顶层启动流程。
 */
    void bootstart_app_init_feedback_devices(void);

    /**
 * @brief 启动周期深睡协调 Application
 *
 * @return ESP_OK 已启动；或初始化、启动错误码
 */
    esp_err_t bootstart_app_start_power_management(void);

    /**
 * @brief 在电源管理完成事件订阅后启动首轮内容刷新
 *
 * 启动失败时先回滚内容刷新资源，再向电源管理提交退避休眠事实。事实提交成功视为错误已经交给
 * 电源 Task 收敛；回滚或事实提交失败则向顶层返回错误，由统一致命错误入口接管。
 *
 * @return ESP_OK 首轮刷新已启动，或失败事实已提交；其他值表示启动失败尚未安全收敛
 */
    esp_err_t bootstart_app_start_content_refresh(void);

    /**
 * @brief 收敛启动阶段致命错误，成功时进入 OTA 回滚重启或退避深睡
 *
 * 本函数先拒绝待验证 OTA 镜像；正常镜像则同步停止已经启动的电源管理与其他运行期组件，并按
 * 1/5/15 分钟失败退避进入深睡。若安全深睡仍无法完成，延迟后重启，绝不把控制权交还给
 * `app_main()`。
 *
 * @param[in] startup_error 启动阶段错误；ESP_OK 会规范化为 ESP_FAIL
 * @return 不返回
 */
    void bootstart_app_handle_fatal_error(esp_err_t startup_error) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
