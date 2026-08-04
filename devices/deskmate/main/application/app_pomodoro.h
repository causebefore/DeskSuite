/**
 * @file app_pomodoro.h
 * @brief 声明本地番茄钟状态机、命令与低功耗补算接口
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "device_button.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 完成音乐逻辑路径最大 UTF-8 字节数，不含结尾 NUL */
#define APP_POMODORO_COMPLETION_AUDIO_PATH_MAX_LENGTH 95U

    /** @brief 番茄钟阶段 */
    typedef enum
    {
        APP_POMODORO_PHASE_NONE = 0,    /**< 尚未开始整组 */
        APP_POMODORO_PHASE_FOCUS,       /**< 专注阶段 */
        APP_POMODORO_PHASE_SHORT_BREAK, /**< 短休阶段 */
        APP_POMODORO_PHASE_LONG_BREAK,  /**< 长休阶段 */
    } app_pomodoro_phase_t;

    /** @brief 番茄钟运行状态 */
    typedef enum
    {
        APP_POMODORO_RUN_STATE_IDLE = 0, /**< 等待开始 */
        APP_POMODORO_RUN_STATE_RUNNING,  /**< 正在倒计时 */
        APP_POMODORO_RUN_STATE_PAUSED,   /**< 已暂停 */
        APP_POMODORO_RUN_STATE_DONE,     /**< 自然到期，等待确认 */
    } app_pomodoro_run_state_t;

    /** @brief 番茄钟可持久化设置 */
    typedef struct
    {
        uint8_t focus_minutes;       /**< 专注时长，5..90，步长 5 */
        uint8_t short_break_minutes; /**< 短休时长，1..30，步长 1 */
        uint8_t long_break_minutes;  /**< 长休时长，5..60，步长 5 */
        uint8_t long_break_interval; /**< 长休间隔，2..8 轮 */
        /** SD 卡内以 `/` 开头的 `.mp3` 逻辑路径 */
        char completion_audio_path[APP_POMODORO_COMPLETION_AUDIO_PATH_MAX_LENGTH + 1U];
    } app_pomodoro_settings_t;

    /** @brief 番茄钟完整领域快照 */
    typedef struct
    {
        app_pomodoro_settings_t  settings;               /**< 当前完整设置 */
        uint64_t                 settings_version;       /**< 当前设置版本，非零且只在采用内存设置时递增 */
        app_pomodoro_phase_t     phase;                  /**< 当前阶段 */
        app_pomodoro_phase_t     next_phase;             /**< DONE 确认后进入的阶段 */
        app_pomodoro_run_state_t run_state;              /**< 当前运行状态 */
        uint32_t                 remaining_seconds;      /**< 向上取整的剩余秒数 */
        uint32_t                 phase_duration_seconds; /**< 当前阶段总秒数 */
        uint8_t                  completed_in_cycle;     /**< 当前长休周期已完成专注数 */
        uint8_t                  today_focus_count;      /**< 已归入可信日期的今日完成数 */
        uint8_t                  pending_focus_count;    /**< 尚未归入可信日期的完成数 */
        bool                     date_verified;          /**< 今日计数是否已有可信本地日期 */
        bool                     settings_saved;         /**< 最近设置和计数写入是否成功 */
        bool                     completion_latched;     /**< 完成提示尚未确认或取消 */
        uint64_t                 completion_generation;  /**< 当前完成提示的非零代次 */
        uint64_t                 generation;             /**< 当前阶段状态代次 */
        bool                     expected_end_valid;     /**< expected_end_utc 是否可展示 */
        time_t                   expected_end_utc;       /**< 可信 UTC 下预计结束时间 */
        esp_err_t                last_error;             /**< 最近一次存储或 Timer 错误 */
    } app_pomodoro_snapshot_t;

    /**
     * @brief 一次基于版本的完整番茄钟设置更新
     *
     * `expected_version` 必须来自此前读取的 `app_pomodoro_snapshot_t.settings_version`。
     */
    typedef struct
    {
        app_pomodoro_settings_t settings;         /**< 待采用的完整设置副本 */
        uint64_t                expected_version; /**< 调用方读取设置时的版本 */
    } app_pomodoro_settings_update_t;

    /** @brief 一次异步番茄钟设置更新的状态 */
    typedef enum
    {
        APP_POMODORO_SETTINGS_UPDATE_STATE_PENDING = 0, /**< 已接受，尚未形成最终结果 */
        APP_POMODORO_SETTINGS_UPDATE_STATE_SUCCEEDED,   /**< 内存设置已采用且 NVS 已保存 */
        APP_POMODORO_SETTINGS_UPDATE_STATE_FAILED,      /**< 已形成失败结果，详情见 error */
    } app_pomodoro_settings_update_state_t;

    /** @brief 一次异步番茄钟设置更新的当前或最终结果 */
    typedef struct
    {
        app_pomodoro_settings_update_state_t state;   /**< 当前请求状态 */
        uint64_t                             version; /**< 当前或最终设置版本 */
        esp_err_t                            error;   /**< PENDING/SUCCEEDED 为 ESP_OK；FAILED 为最终错误 */
    } app_pomodoro_settings_update_result_t;

    /** @brief Light-sleep 返回后的番茄钟补算结果 */
    typedef enum
    {
        APP_POMODORO_WAKEUP_NO_CHANGE = 0,   /**< 无运行阶段或尚未到期 */
        APP_POMODORO_WAKEUP_RESCHEDULED,     /**< 仍在运行，已重新调度阶段 Timer */
        APP_POMODORO_WAKEUP_PHASE_COMPLETED, /**< 本次补算使阶段自然完成 */
    } app_pomodoro_wakeup_result_t;

    /**
     * @brief 初始化番茄钟资源并恢复设置与计数
     *
     * 本函数不创建业务 Task；运行中阶段不会从 NVS 恢复，初始化快照固定为 IDLE。
     *
     * @return ESP_OK 已初始化；ESP_ERR_INVALID_STATE 已初始化；ESP_ERR_NO_MEM 资源不足；
     *         或 NVS、Timer、系统时钟回调注册错误
     */
    esp_err_t app_pomodoro_init(void);

    /**
     * @brief 启动番茄钟 Application Task 并等待初始快照发布
     *
     * @return ESP_OK 已运行；ESP_ERR_INVALID_STATE 生命周期不允许；ESP_ERR_NO_MEM Task 创建失败；
     *         ESP_ERR_TIMEOUT 初始快照未及时发布
     */
    esp_err_t app_pomodoro_start(void);

    /**
     * @brief 协作停止番茄钟 Application Task
     *
     * 超时返回时 Task 仍在后台收敛，调用方应再次调用本函数；成功返回才表示 Task
     * 已退出并且队列已复位。
     *
     * @param[in] timeout_ms 等待 Task 退出的上限，单位毫秒，必须大于 0
     * @return ESP_OK 已停止；ESP_ERR_INVALID_ARG 超时为 0；ESP_ERR_INVALID_STATE 未运行；
     *         ESP_ERR_TIMEOUT 命令或退出等待超时
     */
    esp_err_t app_pomodoro_stop(uint32_t timeout_ms);

    /**
     * @brief 释放已停止的番茄钟队列、Timer 和同步资源
     *
     * @return ESP_OK 已释放；ESP_ERR_INVALID_STATE 未初始化或仍在运行
     */
    esp_err_t app_pomodoro_deinit(void);

    /** @brief 异步请求从 IDLE 开始专注；@return ESP_OK 已入队，或状态/队列错误 */
    esp_err_t app_pomodoro_request_start(void);
    /** @brief 异步请求在 RUNNING 与 PAUSED 间切换；@return ESP_OK 已入队，或状态/队列错误 */
    esp_err_t app_pomodoro_request_toggle_pause(void);
    /** @brief 异步请求跳过当前 RUNNING 阶段；@return ESP_OK 已入队，或状态/队列错误 */
    esp_err_t app_pomodoro_request_skip(void);
    /** @brief 异步请求确认 DONE 并开始下一阶段；@return ESP_OK 已入队，或状态/队列错误 */
    esp_err_t app_pomodoro_request_confirm(void);
    /** @brief 异步请求从 PAUSED 或 DONE 重置整组；@return ESP_OK 已入队，或状态/队列错误 */
    esp_err_t app_pomodoro_request_reset(void);

    /**
     * @brief 异步请求播放当前完成音乐一次
     *
     * 仅供番茄钟处于 IDLE 时执行音频自检。返回成功只表示命令已入队；实际播放结果由
     * Audio Service 日志与事件报告。
     *
     * @return ESP_OK 已入队；ESP_ERR_INVALID_STATE 未运行或正在停止；ESP_ERR_TIMEOUT 队列已满
     */
    esp_err_t app_pomodoro_request_play_completion_audio(void);

    /**
     * @brief 同步校验一个完整番茄钟设置更新
     *
     * 本函数只执行有界参数、当前设置版本、IDLE 状态和单 pending 检查，不修改状态或访问 NVS。
     * 校验成功不构成提交保证；请求接受点和业务 Task 执行点仍会原子地重新校验。
     *
     * @param[in] update 调用期间借用的完整设置更新
     * @return ESP_OK 当前可以提交；ESP_ERR_INVALID_ARG 参数或设置范围非法；
     *         ESP_ERR_INVALID_VERSION 设置版本冲突；ESP_ERR_INVALID_STATE 未运行、不是 IDLE、
     *         已有 pending 请求或设置版本/请求 ID 已耗尽
     */
    esp_err_t app_pomodoro_validate_settings_update(const app_pomodoro_settings_update_t *update);

    /**
     * @brief 异步提交完整番茄钟设置更新副本
     *
     * 本函数在设置状态锁内重新校验版本、IDLE 和单 pending 约束，并以零等待方式把命令复制
     * 入队。返回成功只表示请求已接受；最终 NVS 结果必须通过
     * `app_pomodoro_get_settings_update_result_copy()` 查询。
     *
     * 请求 ID 在一次 Application 生命周期内从 1 严格递增，不回绕或复用。每个终态至少保留
     * 到下一请求成功接受；同步拒绝不产生请求 ID。
     *
     * @param[in] update 调用期间借用的完整设置更新
     * @param[out] out_request_id 成功时写入非零请求 ID
     * @return ESP_OK 请求已接受；ESP_ERR_INVALID_ARG 参数、范围或输出无效；
     *         ESP_ERR_INVALID_VERSION 设置版本冲突；ESP_ERR_INVALID_STATE 未运行、不是 IDLE、
     *         已有 pending 请求或版本/请求 ID 已耗尽；ESP_ERR_TIMEOUT 队列已满
     */
    esp_err_t app_pomodoro_request_update_settings_copy(
        const app_pomodoro_settings_update_t *update,
        uint64_t *out_request_id);

    /**
     * @brief 复制一次已接受番茄钟设置更新的当前结果
     *
     * 同一时刻只保留当前 pending 或最近一个终态。NVS 失败不会回滚已经采用的内存设置，
     * 此时返回 FAILED、真实 NVS 错误和已经递增的新设置版本。
     *
     * @param[in] request_id 请求接受时返回的非零 ID
     * @param[out] out_result 当前或最终结果副本
     * @return ESP_OK 结果有效；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化；
     *         ESP_ERR_NOT_FOUND 请求未知或已被下一请求淘汰
     */
    esp_err_t app_pomodoro_get_settings_update_result_copy(
        uint64_t request_id,
        app_pomodoro_settings_update_result_t *out_result);

    /**
     * @brief 复制最新番茄钟快照
     *
     * @param[out] out_snapshot 调用方提供的完整快照输出
     * @return ESP_OK 已复制；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t app_pomodoro_get_snapshot_copy(app_pomodoro_snapshot_t *out_snapshot);

    /**
     * @brief 查询当前是否需要保持番茄钟前台秒级刷新
     *
     * 仅在番茄钟处于 RUNNING 且当前页面就是番茄钟页时返回 true。该只读产品事实供电源
     * Application 决定只停网络还是进入完整 Light-sleep，不修改页面或番茄钟状态。
     *
     * @return true 应保持 UI 和一秒刷新运行；false 可以按普通策略进入 Light-sleep
     */
    bool app_pomodoro_requires_live_display(void);

    /**
     * @brief 读取运行阶段距离单调截止的相对毫秒数
     *
     * @param[out] out_interval_ms 剩余间隔，至少为 1
     * @return ESP_OK 正在运行；ESP_ERR_NOT_FOUND 当前不是 RUNNING；
     *         ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t app_pomodoro_get_next_wakeup_interval_ms(uint32_t *out_interval_ms);

    /**
     * @brief 在 Power Application 上下文同步补算睡眠期间经过的时间
     *
     * 本函数通过唯一同步命令槽串行化调用；返回前状态和 Presenter 快照均已收敛。
     *
     * @param[in] timeout_ms 等待补算完成的上限，单位毫秒，必须大于 0
     * @param[out] out_result 本次补算结果，仅在 ESP_OK 时有效
     * @return ESP_OK 已补算；ESP_ERR_INVALID_ARG 参数非法；ESP_ERR_INVALID_STATE 未运行；
     *         ESP_ERR_TIMEOUT 队列或回执超时
     */
    esp_err_t app_pomodoro_reconcile_after_wakeup(uint32_t timeout_ms, app_pomodoro_wakeup_result_t *out_result);

    /**
     * @brief 解释番茄钟页面长按输入
     *
     * 左右短按始终返回 false，交由全局页面导航处理。
     *
     * @param[in] key_event 物理按键事件
     * @return true 已消费；false 应继续走全局导航
     */
    bool app_pomodoro_consume_input(device_button_event_t key_event);

#ifdef __cplusplus
}
#endif
