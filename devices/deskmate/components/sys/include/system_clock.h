#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 默认本地时区相对 UTC 的偏移分钟数（中国标准时间） */
#define SYSTEM_CLOCK_DEFAULT_UTC_OFFSET_MINUTES 480

/**
 * @brief SNTP 大跳变候选尚需第二个相近样本确认
 *
 * system_clock_set_time() 返回本错误时不会接受候选时间。调用方应立即取得另一个
 * 连续的第二个 SNTP 样本再次提交，不应将其当成永久故障或已完成校时。当前配置只使用
 * 一个 NTP Server，因此该确认只过滤瞬时异常，不代表独立来源交叉验证。
 */
#define SYSTEM_CLOCK_ERR_CONFIRMATION_REQUIRED  ESP_ERR_NOT_ALLOWED

    /** @brief 系统时间最近一次成功校准所使用的可信来源 */
    typedef enum
    {
        SYSTEM_CLOCK_SOURCE_NONE = 0, /**< 尚无可信时间来源 */
        SYSTEM_CLOCK_SOURCE_RTC,      /**< 板级 RTC 日历时间 */
        SYSTEM_CLOCK_SOURCE_SNTP,     /**< SNTP 网络校时 */
    } system_clock_source_t;

    /** @brief 系统时钟事件 */
    typedef enum
    {
        SYSTEM_CLOCK_EVENT_UPDATED = 1, /**< 已接受 RTC 或 SNTP 可信时间 */
    } system_clock_event_t;

    /** @brief 系统时钟的值语义快照 */
    typedef struct
    {
        time_t                utc_timestamp;      /**< 当前 UTC 时间戳 */
        int16_t               utc_offset_minutes; /**< 从系统存储恢复的 UTC 偏移分钟数 */
        system_clock_source_t source;             /**< 最近一次成功校准来源 */
        bool                  valid;              /**< 当前时间是否可用于产品调度 */
    } system_clock_snapshot_t;

    /**
     * @brief 系统时钟变化回调
     *
     * 回调在接受可信时间的调用者上下文执行。snapshot 只在回调期间有效，回调不得保存其
     * 地址或执行无界阻塞。
     */
    typedef void (*system_clock_callback_t)(system_clock_event_t event, const system_clock_snapshot_t *snapshot,
                                            void *ctx);

    /**
     * @brief 注册长期借用的系统时钟变化回调
     * @param[in] callback 回调函数
     * @param[in] context 原样传回回调函数的上下文，由调用方保证有效期
     * @return ESP_OK 已注册或原本已注册；ESP_ERR_INVALID_ARG 回调为空；
     *         ESP_ERR_INVALID_STATE 系统时钟尚未初始化；ESP_ERR_NO_MEM 回调槽已满
     */
    esp_err_t system_clock_register_callback_borrow(system_clock_callback_t callback, void *context);

    /**
     * @brief 注销此前注册的系统时钟变化回调
     * @param[in] callback 回调函数
     * @param[in] context 注册时借用的上下文
     * @return ESP_OK 已注销；ESP_ERR_INVALID_ARG 回调为空；ESP_ERR_NOT_FOUND 未注册
     */
    esp_err_t system_clock_unregister_callback(system_clock_callback_t callback, void *context);

    /**
 * @brief 初始化系统时钟
 *
 * 初始化时从系统存储恢复 UTC 偏移，并把 C 运行库本地时区配置为固定 UTC+8；
 * 当前产品固定为 +480 分钟，尚未保存或存量值不是 +480 时都会归一化并尝试写回。
 * 初始化本身不读取硬件时钟，调用方应在 `device_rtc_init()` 后调用
 * `system_clock_sync_from_rtc()`。
 *
 * @return ESP_OK 成功，或其他错误码
 */
    esp_err_t system_clock_init(void);

    /**
 * @brief 获取当前系统时间
 *
 * @param[out] timestamp 当前 UTC 时间戳
 *
 * @return ESP_OK 成功，或其他错误码
 */
    esp_err_t system_clock_get_time(time_t *timestamp);

    /**
 * @brief 复制获取当前系统时钟快照
 *
 * 时间尚不可信时仍返回 ESP_OK，并将 valid 设为 false、source 设为
 * SYSTEM_CLOCK_SOURCE_NONE。调用方必须先检查 valid，才能将 utc_timestamp
 * 用于自动刷新或静默时段判断。
 *
 * @param[out] out_snapshot 系统时钟快照输出指针
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 系统时钟尚未初始化；或系统时钟读取错误码
 */
    esp_err_t system_clock_get_snapshot_copy(system_clock_snapshot_t *out_snapshot);

    /**
 * @brief 使用可信 SNTP 时间设置系统时钟
 *
 * 仅接受 2024-01-01 00:00:00 至 2099-12-31 23:59:59 范围内的 UTC
 * 时间。接口名称为兼容既有调用方而保留，成功后时间来源记为 SNTP，并尽力把
 * UTC+8 日历时间回写设备 RTC；RTC 回写失败会记录警告，但不撤销已经接受的系统时间。
 *
 * @param[in] timestamp 经 SNTP 校准的 UTC 时间戳
 *
 * 已有可信时间时，候选时间若与按单调时钟推进的当前时间相差超过 6 小时，
 * 必须连续取得两个相差不超过 5 分钟的 SNTP 样本后才会接受。
 *
 * @return ESP_OK 系统时间设置成功；ESP_ERR_INVALID_ARG 时间超出可信范围；
 *         SYSTEM_CLOCK_ERR_CONFIRMATION_REQUIRED 大跳变尚需第二个样本确认；
 *         或系统时钟相关错误码
 */
    esp_err_t system_clock_set_time(time_t timestamp);

    /**
 * @brief 同步向指定 SNTP 服务器取时并更新系统时钟
 *
 * 本函数只能在网络已经上线后调用。每个样本最多等待 timeout_ms；若候选时间相对当前可信
 * 时间跳变超过 6 小时，会主动请求第二个样本确认，因此最坏阻塞时间约为两倍 timeout_ms。
 * 函数返回前会停止本次 SNTP 客户端；成功接受时间后由 system_clock_set_time() 尽力回写 RTC。
 *
 * @param[in] server 非空 SNTP 服务器域名或 IP，仅在调用期间借用
 * @param[in] timeout_ms 单个 SNTP 样本等待上限，单位毫秒
 * @return ESP_OK 校时成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_TIMEOUT 等待超时；
 *         SYSTEM_CLOCK_ERR_CONFIRMATION_REQUIRED 两个大跳变样本不一致；或底层错误码
 */
    esp_err_t system_clock_sync_from_sntp(const char *server, uint32_t timeout_ms);

    /**
 * @brief 从已经初始化的设备 RTC 同步系统时钟
 *
 * RTC 日历按当前固定 UTC+8 解释。电压过低标志置位时拒绝接受该时间。
 *
 * @return ESP_OK 同步成功；ESP_ERR_INVALID_STATE 系统时钟或 RTC 未初始化、RTC 时间不可信；
 *         ESP_ERR_INVALID_RESPONSE RTC 日历字段无效；或底层错误码
 */
    esp_err_t system_clock_sync_from_rtc(void);

    /**
 * @brief 判断当前系统时间是否可信
 *
 * @return true 当前时间已由可信来源校准且仍处于有效范围，false 否则
 */
    bool system_clock_is_valid(void);

#ifdef __cplusplus
}
#endif
