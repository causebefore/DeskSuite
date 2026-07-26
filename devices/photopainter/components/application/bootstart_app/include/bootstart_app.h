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
 * @brief 初始化本地集合、页面呈现、按键播放和可选内容刷新链路
 *
 * 本函数会启动照片播放 App，但只初始化内容刷新 App；首轮内容刷新必须等待电源管理完成回调
 * 订阅后再由 `bootstart_app_start_content_refresh()` 启动。
 *
 * @param[out] out_refresh_ready true 表示内容刷新 App 已初始化并等待启动
 * @return ESP_OK 本地播放已启动；ESP_ERR_INVALID_ARG 参数为空；或关键显示链路错误码
 */
    esp_err_t bootstart_app_start_photo_pipeline(bool *out_refresh_ready);

    /**
 * @brief 尽力初始化 LED 与蜂鸣器反馈能力
 *
 * 失败仅记录诊断，不阻断顶层启动流程。
 */
    void bootstart_app_init_feedback_devices(void);

    /**
 * @brief 启动手动与周期深睡协调 Application
 *
 * @param[in] automatic_sleep_enabled 是否启用自动深睡流程
 * @return ESP_OK 已启动；或初始化、启动错误码
 */
    esp_err_t bootstart_app_start_power_management(bool automatic_sleep_enabled);

    /**
 * @brief 在电源管理完成事件订阅后启动首轮内容刷新
 *
 * `refresh_ready=false` 时直接返回；启动失败时会回滚内容刷新资源并向电源管理提交退避休眠
 * 事实，所有错误均在函数内记录。
 *
 * @param[in] refresh_ready 内容刷新 App 是否已经初始化
 */
    void bootstart_app_start_content_refresh(bool refresh_ready);

    /**
 * @brief 在启动阶段致命故障发生于待验证镜像时请求 OTA 回滚并重启
 *
 * @param[in] startup_error 启动阶段错误；ESP_OK 时不执行操作
 */
    void bootstart_app_reject_pending_image_on_fatal_error(esp_err_t startup_error);

#ifdef __cplusplus
}
#endif
