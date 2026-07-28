/**
 * @file bsp.h
 * @brief ESP32-S3-RLCD-4.2 板级资源与外设装配接口
 *
 * BSP 读取当前 Board 的静态参数，创建 ESP-IDF 总线和 Driver 实例，并向 Device 提供
 * 同步、与产品策略无关的硬件操作。BSP 不负责产品启动顺序，也不提供全局设备注册表。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief BSP 对外可识别的两个实体按键 */
    typedef enum
    {
        BSP_BUTTON_LEFT = 0,
        BSP_BUTTON_RIGHT,
        BSP_BUTTON_COUNT,
    } bsp_button_id_t;

    /** @brief GPIO ISR 上下文中的按键电平活动回调 */
    typedef void (*bsp_button_activity_callback_t)(void *context);

    /** @brief 电池 ADC 的单次板级采样结果 */
    typedef struct
    {
        uint16_t raw;        /**< ADC 原始值 */
        uint16_t pin_mv;     /**< ADC 引脚电压，单位毫伏 */
        uint16_t battery_mv; /**< 应用板级分压参数后的电池电压，单位毫伏 */
    } bsp_battery_sample_t;

    /** @brief 已应用板级校准的温湿度采样 */
    typedef struct
    {
        int16_t  temperature_centi; /**< 温度，单位 0.01°C */
        uint16_t humidity_centi;    /**< 相对湿度，单位 0.01% */
    } bsp_environment_sample_t;

    /** @brief RTC 使用的本地日历时间 */
    typedef struct
    {
        uint16_t year;
        uint8_t  month;
        uint8_t  day;
        uint8_t  hour;
        uint8_t  minute;
        uint8_t  second;
    } bsp_rtc_datetime_t;

    /** @brief RTC 告警参与匹配的字段 */
    typedef enum
    {
        BSP_RTC_ALARM_MATCH_SECOND  = 1U << 0,
        BSP_RTC_ALARM_MATCH_MINUTE  = 1U << 1,
        BSP_RTC_ALARM_MATCH_HOUR    = 1U << 2,
        BSP_RTC_ALARM_MATCH_DAY     = 1U << 3,
        BSP_RTC_ALARM_MATCH_WEEKDAY = 1U << 4,
        BSP_RTC_ALARM_MATCH_ALL     = (1U << 5) - 1U,
    } bsp_rtc_alarm_match_t;

    /** @brief RTC 告警比较配置 */
    typedef struct
    {
        uint8_t second;
        uint8_t minute;
        uint8_t hour;
        uint8_t day;
        uint8_t weekday;
        uint8_t match_fields; /**< `bsp_rtc_alarm_match_t` 位组合 */
    } bsp_rtc_alarm_t;

    /**
     * @brief RTC INT 快速通知回调
     *
     * 回调在 GPIO ISR 上下文执行，只能调用 ISR-safe API，必须快速返回。
     */
    typedef void (*bsp_rtc_interrupt_callback_t)(void *context);

    /** @brief 显示面板的静态能力快照 */
    typedef struct
    {
        uint16_t width_pixels;
        uint16_t height_pixels;
    } bsp_display_info_t;

    /** @brief SD 卡的板级块设备信息 */
    typedef struct
    {
        uint32_t sector_count;      /**< 可访问扇区总数 */
        uint32_t sector_size_bytes; /**< 单个扇区大小，单位字节 */
        uint64_t capacity_bytes;    /**< 总容量，单位字节 */
    } bsp_storage_info_t;

    /** @brief 一次板级轻睡眠事务的唤醒结果 */
    typedef struct
    {
        bool left_button;  /**< 左键 GPIO 导致唤醒 */
        bool right_button; /**< 右键 GPIO 导致唤醒 */
        bool timer;        /**< ESP32 内部 Timer 导致唤醒 */
    } bsp_power_wakeup_result_t;

    /** @brief 初始化按键 GPIO 资源 */
    esp_err_t bsp_button_init(void);

    /** @brief 释放按键 GPIO 资源 */
    esp_err_t bsp_button_deinit(void);

    /**
 * @brief 读取指定实体按键的原始电平
 * @param[in] button 按键标识
 * @param[out] out_high true 表示当前为高电平
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t bsp_button_read_level(bsp_button_id_t button, bool *out_high);

    /**
     * @brief 注册或清除长期借用的按键双边沿活动回调
     *
     * 非 NULL 回调运行在 GPIO ISR 上下文，只允许执行 ISR-safe 的有界操作。
     * 传入 NULL 时先关闭左右键中断，再清除借用的回调和上下文。
     *
     * @param[in] callback 活动回调；NULL 表示清除
     * @param[in] context 长期借用的回调上下文；清除时忽略
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE BSP 尚未初始化；或 GPIO 驱动错误码
     */
    esp_err_t bsp_button_set_activity_callback_borrow(bsp_button_activity_callback_t callback, void *context);

    /**
     * @brief 以左右按键和内部 Timer 为唤醒源执行一次完整轻睡眠事务
     *
     * 本函数先确认左右按键均已释放，再配置 EXT1 低电平与内部 Timer 唤醒并阻塞当前调用
     * Task。任一唤醒源命中后从原调用点返回，不发生芯片复位，也不会重新执行 app_main()。
     * 函数返回前会清理本轮临时唤醒配置，不向调用方暴露半准备状态。
     *
     * @param[in] timer_wakeup_ms Timer 唤醒间隔，单位毫秒，必须大于 0
     * @param[out] out_result 本次事务的唤醒结果，仅在 ESP_OK 时有效
     * @return ESP_OK 已从轻睡眠唤醒且清理完成；ESP_ERR_INVALID_ARG 参数无效；
     *         ESP_ERR_INVALID_STATE 按键尚未释放；或底层配置、睡眠及清理错误码
     */
    esp_err_t bsp_power_enter_light_sleep(uint32_t timer_wakeup_ms, bsp_power_wakeup_result_t *out_result);

    /** @brief 初始化电池 ADC 资源 */
    esp_err_t bsp_battery_init(void);

    /** @brief 释放电池 ADC 资源 */
    esp_err_t bsp_battery_deinit(void);

    /**
 * @brief 同步读取一次电池 ADC
 * @param[out] out_sample 板级采样结果，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层 ADC 错误码
 */
    esp_err_t bsp_battery_read_sample(bsp_battery_sample_t *out_sample);

    /** @brief 初始化环境传感器及其 I2C Driver 实例 */
    esp_err_t bsp_environment_init(void);

    /** @brief 释放环境传感器 Driver 实例 */
    esp_err_t bsp_environment_deinit(void);

    /**
 * @brief 同步读取一次已校准温湿度
 * @param[out] out_sample 采样结果，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
 */
    esp_err_t bsp_environment_read_sample(bsp_environment_sample_t *out_sample);

    /** @brief 初始化 RTC 及其 I2C Driver 实例 */
    esp_err_t bsp_rtc_init(void);

    /** @brief 释放 RTC Driver 实例但不停止芯片走时 */
    esp_err_t bsp_rtc_deinit(void);

    /**
 * @brief 读取 RTC 日历时间
 * @param[out] out_datetime 日历时间，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
 */
    esp_err_t bsp_rtc_read_datetime(bsp_rtc_datetime_t *out_datetime);

    /** @brief 读取 RTC 振荡停止/电压过低标志 */
    esp_err_t bsp_rtc_read_voltage_low(bool *out_voltage_low);

    /**
 * @brief 设置 RTC 日历时间
 * @param[in] datetime 日历时间，仅在调用期间借用
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
 */
    esp_err_t bsp_rtc_set_datetime(const bsp_rtc_datetime_t *datetime);

    /**
     * @brief 配置 RTC 告警、清除旧标志并启用告警输出
     * @param[in] alarm 告警比较配置，仅在调用期间借用
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；或底层错误码
     */
    esp_err_t bsp_rtc_set_alarm(const bsp_rtc_alarm_t *alarm);

    /**
     * @brief 读取 RTC 告警比较配置
     * @param[out] out_alarm 告警配置，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t bsp_rtc_read_alarm(bsp_rtc_alarm_t *out_alarm);

    /**
     * @brief 启用或关闭 RTC 告警中断
     * @param[in] enabled true 启用，false 关闭
     * @return ESP_OK 成功，或底层错误码
     */
    esp_err_t bsp_rtc_enable_alarm_interrupt(bool enabled);

    /**
     * @brief 读取 RTC 告警中断是否启用
     * @param[out] out_enabled true 表示告警中断已启用，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t bsp_rtc_read_alarm_interrupt_enabled(bool *out_enabled);

    /** @brief 清除 RTC 告警标志，返回底层操作结果 */
    esp_err_t bsp_rtc_clear_alarm_flag(void);

    /**
     * @brief 读取 RTC 告警标志
     * @param[out] out_pending true 表示 AF 已置位，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
     */
    esp_err_t bsp_rtc_read_alarm_flag(bool *out_pending);

    /**
     * @brief 读取板级 RTC INT 是否处于低电平有效状态
     * @param[out] out_asserted true 表示 INT 当前有效，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t bsp_rtc_read_interrupt_asserted(bool *out_asserted);

    /**
     * @brief 设置长期借用的 RTC INT ISR 回调
     *
     * 传入 NULL 清除回调。借用期持续到再次设置、清除或 `bsp_rtc_deinit()`。
     *
     * @param[in] callback ISR 回调，可为 NULL
     * @param[in] context 原样传给回调的上下文
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t bsp_rtc_set_interrupt_callback_borrow(bsp_rtc_interrupt_callback_t callback, void *context);

    /** @brief 初始化 RLCD、帧缓冲和传输资源 */
    esp_err_t bsp_display_init(void);

    /**
     * @brief 同步停止新帧并等待 RLCD 传输静止
     *
     * 本函数保留刷新 Task、SPI、面板控制器和全部缓冲区；返回 ESP_OK 时不会再接受新帧，
     * 已提交 DMA 已完成，LCD 输出脚已保持为 Light-sleep 安全状态。
     *
     * @param[in] timeout_ms 等待在途刷新完成的总超时，单位毫秒
     * @return ESP_OK 已停止或原本已停止；ESP_ERR_INVALID_ARG 超时为零；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或 GPIO、DMA 等底层错误码
     */
    esp_err_t bsp_display_stop(uint32_t timeout_ms);

    /**
     * @brief 恢复已停止的 RLCD 并重新接受显示帧
     *
     * 本函数不复位或重新初始化面板控制器。
     *
     * @return ESP_OK 已运行或原本正在运行；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或 GPIO 恢复错误码
     */
    esp_err_t bsp_display_start(void);

    /** @brief 等待传输完成并释放 RLCD 资源 */
    esp_err_t bsp_display_deinit(void);

    /**
 * @brief 复制显示面板静态尺寸
 * @param[out] out_info 显示信息，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空
 */
    esp_err_t bsp_display_get_info_copy(bsp_display_info_t *out_info);

    /** @brief 把 RGB565 区域同步转换并写入 BSP 内部帧 */
    esp_err_t bsp_display_write_rgb565_area(int x1, int y1, int x2, int y2, const uint16_t *pixels, int stride_pixels);

    /** @brief 把 1 bpp 区域同步转换并写入 BSP 内部帧 */
    esp_err_t bsp_display_write_i1_area(int x1, int y1, int x2, int y2, const uint8_t *pixels, uint32_t stride_bytes);

    /**
 * @brief 请求异步执行一次显示刷新
 *
 * ESP_OK 只表示刷新请求已提交；最终传输结果由 `bsp_display_wait_flush_done()` 返回。
 *
 * @return ESP_OK 请求已提交；ESP_ERR_INVALID_STATE 当前不接受新帧；ESP_ERR_TIMEOUT
 *         刷新请求队列已满
 */
    esp_err_t bsp_display_request_flush(void);

    /** @brief 有界等待已经提交的显示刷新完成 */
    esp_err_t bsp_display_wait_flush_done(uint32_t timeout_ms);

    /** @brief 读取最近一秒显示刷新帧率 */
    uint32_t bsp_display_get_flush_fps(void);

    /** @brief 读取启动后的显示刷新总次数 */
    uint32_t bsp_display_get_total_flush_count(void);

    /**
     * @brief 初始化独立 SPI2 总线并探测板载 SD 卡
     *
     * 本函数在调用者上下文同步完成总线、SDSPI 设备和卡协议初始化，返回时卡已可执行扇区
     * 访问；重复调用成功返回 ESP_OK。BSP 不提供跨 Task 串行化，调用方必须保证初始化、
     * 反初始化和扇区事务不会并发执行。
     *
     * @return ESP_OK 初始化完成；ESP_ERR_INVALID_STATE 存在上次未清理完成的资源；
     *         或 SPI、SDSPI、SD 协议错误码
     */
    esp_err_t bsp_storage_init(void);

    /**
     * @brief 释放 SD 卡 SDSPI 设备和独立 SPI2 总线
     *
     * 本函数同步执行。调用前必须卸载上层文件系统并停止新的扇区事务。若部分清理失败，
     * 未释放的资源状态会被保留，调用方可以再次调用本函数继续清理。
     *
     * @return ESP_OK 已释放或原本未初始化；或底层清理错误码，此时可能仍持有部分资源
     */
    esp_err_t bsp_storage_deinit(void);

    /**
     * @brief 检查已初始化 SD 卡当前是否响应
     *
     * 本函数在调用者上下文同步发送卡状态命令；板上没有 Card Detect 引脚，本接口不提供
     * 热插拔事件或持续监测。
     *
     * @return ESP_OK 卡可用；ESP_ERR_INVALID_STATE 尚未初始化；或 SD 协议错误码
     */
    esp_err_t bsp_storage_check_ready(void);

    /**
     * @brief 复制 SD 卡块设备信息
     *
     * 本函数同步复制初始化时保存的卡信息，不返回内部对象或底层句柄。
     *
     * @param[out] out_info 卡信息，仅在返回 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t bsp_storage_get_info_copy(bsp_storage_info_t *out_info);

    /**
     * @brief 同步读取连续 SD 卡扇区
     *
     * 函数返回前完成底层读取且不保留输出缓冲区指针；调用方负责与其他 BSP 存储事务串行化。
     *
     * @param[in] start_sector 起始扇区编号
     * @param[in] sector_count 连续读取的扇区数，必须大于 0
     * @param[out] out_data 输出缓冲区，容量至少为扇区大小乘扇区数
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或范围无效；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或底层读取错误码
     */
    esp_err_t bsp_storage_read_sectors(uint32_t start_sector, size_t sector_count, void *out_data);

    /**
     * @brief 同步写入连续 SD 卡扇区
     *
     * 函数返回前完成底层写入且不保留输入缓冲区指针；调用方负责与其他 BSP 存储事务串行化。
     *
     * @param[in] start_sector 起始扇区编号
     * @param[in] sector_count 连续写入的扇区数，必须大于 0
     * @param[in] data 输入缓冲区，容量至少为扇区大小乘扇区数
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或范围无效；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或底层写入错误码
     */
    esp_err_t bsp_storage_write_sectors(uint32_t start_sector, size_t sector_count, const void *data);

    /** @brief BSP 音频资源的板级装配参数 */
    typedef struct
    {
        uint32_t sample_rate_hz; /**< 输入输出共用采样率，单位 Hz */
        uint8_t  initial_volume; /**< 初始输出音量，范围 0～100 */
        uint8_t  input_gain_db;  /**< ES7210 输入增益，单位 dB */
    } bsp_audio_config_t;

    /**
     * @brief 初始化 I2S、ES8311 和 ES7210 音频资源
     * @param[in] config 板级音频参数，仅在调用期间借用
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法；ESP_ERR_INVALID_STATE 已初始化；
     *         或底层资源创建错误
     */
    esp_err_t bsp_audio_init(const bsp_audio_config_t *config);

    /** @brief 释放音频输入、输出和 I2S 资源 */
    esp_err_t bsp_audio_deinit(void);

    /** @brief 使能或关闭扬声器输出 */
    esp_err_t bsp_audio_enable_output(bool enable);

    /** @brief 使能或关闭麦克风输入 */
    esp_err_t bsp_audio_enable_input(bool enable);

    /** @brief 设置 0～100 的扬声器输出音量 */
    esp_err_t bsp_audio_set_output_volume(int volume);

    /**
     * @brief 同步写入 16-bit PCM 单声道样本
     * @param[in] data PCM 样本，仅在调用期间借用
     * @param[in] sample_count 请求写入的样本数
     * @param[out] out_written 实际写入样本数，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 输出未启用；或底层写入错误
     */
    esp_err_t bsp_audio_write(const int16_t *data, size_t sample_count, size_t *out_written);

    /**
     * @brief 同步读取 16-bit PCM 样本
     * @param[out] dest PCM 输出缓冲区
     * @param[in] sample_count 请求读取的样本数
     * @param[out] out_read 实际读取样本数，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 输入未启用；或底层读取错误
     */
    esp_err_t bsp_audio_read(int16_t *dest, size_t sample_count, size_t *out_read);

    /** @brief 返回当前已初始化音频资源的采样率，未初始化时返回 0 */
    uint32_t bsp_audio_get_sample_rate_hz(void);

#ifdef __cplusplus
}
#endif
