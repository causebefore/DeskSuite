/**
 * @file app_power.h
 * @brief 编排 DeskMate 按键与内部 Timer Light-sleep 闭环
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 电源 Application 的唯一运行状态 */
    typedef enum
    {
        APP_POWER_STATE_STOPPED = 0, /*!< 尚未启动或已经停止 */
        APP_POWER_STATE_AWAKE,       /*!< 等待无活动窗口或产品阻止条件解除 */
        APP_POWER_STATE_PREPARING,   /*!< 检查活动代次并可逆停止语音、UI 与网络 */
        APP_POWER_STATE_SLEEPING,    /*!< 当前 Task 阻塞在 Light-sleep 入口 */
        APP_POWER_STATE_RESUMING,    /*!< 已唤醒并恢复、刷新 UI */
        APP_POWER_STATE_BLOCKED,     /*!< 睡眠或恢复状态不可靠，禁止再次自动睡眠 */
    } app_power_state_t;

    /** @brief 当前睡眠循环正在执行的显式步骤 */
    typedef enum
    {
        APP_POWER_STEP_NONE = 0,       /*!< 当前没有准备或恢复步骤 */
        APP_POWER_STEP_CHECK_BLOCKERS, /*!< 检查只读产品阻止条件 */
        APP_POWER_STEP_VOICE_STOP,     /*!< 停止语音 Runtime 并关闭业务入口 */
        APP_POWER_STEP_UI_STOP,        /*!< 可逆停止 UI Runtime 和显示传输 */
        APP_POWER_STEP_NETWORK_STOP,   /*!< 可逆停止网络策略与 Wi-Fi Driver */
        APP_POWER_STEP_DEVICE_SLEEP,   /*!< 执行 Device 按键与 Timer 睡眠事务 */
        APP_POWER_STEP_DEVICE_WAKE,    /*!< 锁存本轮 Light-sleep 唤醒事实 */
        APP_POWER_STEP_NETWORK_START,  /*!< 恢复网络连接策略 */
        APP_POWER_STEP_VOICE_START,    /*!< 恢复语音 Runtime 和按键语音入口 */
        APP_POWER_STEP_NETWORK_SYNC,   /*!< 等待联网并同步 Dashboard 维护数据 */
        APP_POWER_STEP_UI_START,       /*!< 恢复 UI Runtime 并同步完成画面刷新 */
    } app_power_step_t;

    /** @brief 最近一次 Light-sleep 返回时锁存的语义化唤醒来源 */
    typedef enum
    {
        APP_POWER_WAKEUP_NONE = 0,     /*!< 尚未完成睡眠或本轮未产生唤醒事实 */
        APP_POWER_WAKEUP_LEFT_BUTTON,  /*!< 左键唤醒 */
        APP_POWER_WAKEUP_RIGHT_BUTTON, /*!< 右键唤醒 */
        APP_POWER_WAKEUP_BOTH_BUTTONS, /*!< 左右键同时命中 EXT1 状态 */
        APP_POWER_WAKEUP_TIMER,        /*!< ESP32 内部 Timer 唤醒 */
        APP_POWER_WAKEUP_UNKNOWN,      /*!< 返回成功但没有有效唤醒来源 */
    } app_power_wakeup_source_t;

    /** @brief 暂时阻止自动睡眠的只读产品状态位 */
    typedef enum
    {
        APP_POWER_BLOCKER_NONE          = 0U,
        APP_POWER_BLOCKER_VOICE         = 1U << 0,
        APP_POWER_BLOCKER_AUDIO         = 1U << 1,
        APP_POWER_BLOCKER_OTA           = 1U << 2,
        APP_POWER_BLOCKER_NETWORK_LEASE = 1U << 3,
        APP_POWER_BLOCKER_AUDIO_PROCESSOR = 1U << 4,
    } app_power_blocker_t;

    /** @brief 电源 Application 初始化配置 */
    typedef struct
    {
        bool     automatic_light_sleep_enabled; /*!< 是否启用自动 Light-sleep */
        uint32_t idle_timeout_ms;               /*!< 用户无活动窗口，单位毫秒 */
        uint32_t refresh_interval_ms;           /*!< 内部 Timer 唤醒刷新间隔，单位毫秒 */
        uint32_t retry_delay_ms;                /*!< 暂时不可睡眠时的重试间隔，单位毫秒 */
    } app_power_config_t;

    /** @brief 电源 Application 只读状态快照 */
    typedef struct
    {
        app_power_state_t         state;                         /*!< 当前主状态 */
        app_power_step_t          step;                          /*!< 当前显式步骤 */
        app_power_wakeup_source_t wakeup_source;                 /*!< 最近唤醒来源 */
        bool                      automatic_light_sleep_enabled; /*!< 自动睡眠开关 */
        uint32_t                  activity_generation;           /*!< 已接收用户活动代次 */
        uint32_t                  cycle_id;                      /*!< 睡眠尝试编号 */
        uint32_t                  success_count;                 /*!< 按键唤醒并恢复交互次数 */
        uint32_t                  timer_refresh_count;           /*!< Timer 唤醒并刷新屏幕次数 */
        uint32_t                  blockers;                      /*!< app_power_blocker_t 位组合 */
        esp_err_t                 primary_error;                 /*!< 最近主操作错误 */
        esp_err_t                 recovery_error;                /*!< 最近诊断或恢复错误 */
    } app_power_status_t;

    /**
     * @brief 初始化电源 Application 的配置和停止同步资源
     *
     * 本函数不创建 Task。调用前只需保证状态查询所依赖的 Application 与 Service 已经初始化。
     *
     * @param[in] config 初始化配置
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 配置无效；ESP_ERR_INVALID_STATE 重复初始化；
     *         ESP_ERR_NO_MEM 资源不足
     */
    esp_err_t app_power_init(const app_power_config_t *config);

    /**
     * @brief 启动唯一的按键与内部 Timer Light-sleep 编排 Task
     *
     * 无活动窗口结束后，Task 可逆停止 UI 与网络并进入轻睡眠。Timer 唤醒只在服务端截止
     * 时间到达时恢复网络、同步 Dashboard，再停网并刷新一次屏幕后继续睡眠；左右按键唤醒
     * 则按网络、语音、UI 的顺序恢复正常交互窗口。
     *
     * @return ESP_OK 已启动；ESP_ERR_INVALID_STATE 生命周期不允许；ESP_ERR_NO_MEM 创建失败
     */
    esp_err_t app_power_start(void);

    /**
     * @brief 报告一次用户活动并重置无活动窗口
     *
     * 可从普通 Task 或 ESP Event Loop 上下文调用；只更新小型状态并通知电源 Task。
     *
     * @return ESP_OK 已记录；ESP_ERR_INVALID_STATE 尚未启动
     */
    esp_err_t app_power_notify_activity(void);

    /**
     * @brief 复制完整电源 Application 状态
     *
     * @param[out] out_status 状态输出
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t app_power_get_status_copy(app_power_status_t *out_status);

    /**
     * @brief 同步请求停止电源 Task
     *
     * 若 Task 已进入 Light-sleep，本函数最迟等待下一次内部 Timer 唤醒后完成停止。
     *
     * @param[in] timeout_ms 最长等待时间，单位毫秒
     * @return ESP_OK 已停止；ESP_ERR_INVALID_ARG 超时为零；
     *         ESP_ERR_INVALID_STATE 未运行；ESP_ERR_TIMEOUT 未及时退出
     */
    esp_err_t app_power_stop(uint32_t timeout_ms);

    /**
     * @brief 释放已经停止的电源 Application 资源
     *
     * @return ESP_OK 已释放；ESP_ERR_INVALID_STATE 尚未初始化或仍在运行
     */
    esp_err_t app_power_deinit(void);

#ifdef __cplusplus
}
#endif
