/**
 * @file pomodoro_store.h
 * @brief 声明番茄钟设置与本地完成计数的 NVS 持久化接口
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 当前番茄钟持久化格式版本 */
#define POMODORO_STORE_SCHEMA_VERSION                   2U

    /** @brief 完成音乐逻辑路径最大 UTF-8 字节数，不含结尾 NUL */
#define POMODORO_STORE_COMPLETION_AUDIO_PATH_MAX_LENGTH 95U

    /** @brief 无持久化值或迁移旧 schema 时使用的完成音乐逻辑路径 */
#define POMODORO_STORE_DEFAULT_COMPLETION_AUDIO_PATH "/pomodoro-complete.mp3"

    /** @brief 番茄钟持久化设置 */
    typedef struct
    {
        uint8_t focus_minutes;       /**< 专注时长，5..180 分钟，步长 1 */
        uint8_t short_break_minutes; /**< 短休时长，5..180 分钟，步长 1 */
        uint8_t long_break_minutes;  /**< 长休时长，5..180 分钟，步长 1 */
        uint8_t long_break_interval; /**< 进入长休前需要完成的专注轮数，2..12，步长 1 */
        /** SD 卡内以 `/` 开头的 `.mp3` 逻辑路径 */
        char completion_audio_path[POMODORO_STORE_COMPLETION_AUDIO_PATH_MAX_LENGTH + 1U];
    } pomodoro_store_settings_t;

    /** @brief 番茄钟持久化快照 */
    typedef struct
    {
        pomodoro_store_settings_t settings;           /**< 原样读取的设置 */
        uint32_t                  today_date;         /**< 本地日期戳 YYYYMMDD */
        uint8_t                   today_count;        /**< 已归入 today_date 的完成数 */
        uint8_t                   pending_count;      /**< 尚未归入可信日期的完成数 */
        bool                      schema_valid;       /**< 格式版本是否为当前版本 */
        bool                      migration_required; /**< 是否从兼容旧格式读取并需要迁移 */
        bool                      settings_valid;     /**< 全部设置是否存在且合法 */
        bool                      counts_valid;       /**< 三个计数字段是否缺失或类型正确 */
    } pomodoro_store_snapshot_t;

    /**
     * @brief 初始化番茄钟存储
     *
     * 本函数只验证 NVS 命名空间可访问，不创建 Task 或 Timer；重复调用安全。
     *
     * @return ESP_OK 可访问；其他值表示 NVS 打开失败
     */
    esp_err_t pomodoro_store_init(void);

    /**
     * @brief 校验一份完整番茄钟持久化设置
     *
     * @param[in] settings 调用期间借用的完整设置
     * 三项时长均接受 5..180 范围内任意整数分钟，长休间隔接受 2..12 范围内任意整数轮。
     *
     * @return true 时长范围和完成音乐 UTF-8 逻辑路径均合法；false 设置无效
     */
    bool pomodoro_store_settings_are_valid(const pomodoro_store_settings_t *settings);

    /**
     * @brief 按值读取番茄钟持久化快照
     *
     * 缺失或非法版本、设置通过快照中的有效性字段报告；计数字段缺失时按 0 返回。
     *
     * @param[out] out_snapshot 调用方提供的完整快照输出
     * @return ESP_OK 已完成读取；ESP_ERR_INVALID_ARG 参数为空；其他值表示 NVS 错误
     */
    esp_err_t pomodoro_store_load_copy(pomodoro_store_snapshot_t *out_snapshot);

    /**
     * @brief 原子提交格式版本和完整设置副本
     *
     * @param[in] settings 调用期间借用的完整设置
     * @return ESP_OK 已提交；ESP_ERR_INVALID_ARG 参数或范围非法；其他值表示 NVS 错误
     */
    esp_err_t pomodoro_store_save_settings_copy(const pomodoro_store_settings_t *settings);

    /**
     * @brief 原子提交今日日期、今日完成数和未定日完成数
     *
     * @param[in] today_date 本地日期戳 YYYYMMDD；尚无可信日期时可为 0
     * @param[in] today_count 已归入日期的完成数
     * @param[in] pending_count 尚未归入可信日期的完成数
     * @return ESP_OK 已提交；其他值表示 NVS 错误
     */
    esp_err_t pomodoro_store_save_counts(uint32_t today_date, uint8_t today_count, uint8_t pending_count);

#ifdef __cplusplus
}
#endif
