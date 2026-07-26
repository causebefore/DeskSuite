/**
 * @file bsp.h
 * @brief BSP 板级公共能力接口
 *
 * 提供板级外设的初始化与控制。上层（Device）只通过此头文件访问 BSP 能力，
 * 不直接包含 board_pin_defs.h 或 Driver 头文件。
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

    /** @brief 板载环境传感器的一次测量结果 */
    typedef struct
    {
        float temperature_c;
        float humidity_percent;
    } bsp_environment_measurement_t;

    /** @brief 写入板载 RTC 的日历时间 */
    typedef struct
    {
        uint16_t year;
        uint8_t  month;
        uint8_t  day;
        uint8_t  hour;
        uint8_t  minute;
        uint8_t  second;
    } bsp_rtc_datetime_t;

    /** @brief 从板载 RTC 复制出的完整时间快照 */
    typedef struct
    {
        bsp_rtc_datetime_t datetime;
        uint8_t            weekday;
        bool               voltage_low;
    } bsp_rtc_snapshot_t;

    /**
 * @brief 初始化板载环境传感器
 *
 * 首次使用共享 I2C 总线时会先创建并复位总线，然后复位传感器并校验序列号。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或底层错误码
 */
    esp_err_t bsp_environment_init(void);

    /**
 * @brief 读取板载环境传感器序列号
 *
 * @param[out] out_serial_number 32 位序列号，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t bsp_environment_get_serial_number(uint32_t *out_serial_number);

    /**
 * @brief 同步执行一次高精度温湿度测量
 *
 * 本函数同步阻塞约 9 ms。
 *
 * @param[out] out_measurement 测量结果，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化；
 *         或底层错误码
 */
    esp_err_t bsp_environment_measure(bsp_environment_measurement_t *out_measurement);

    /**
 * @brief 释放板载环境传感器及其共享总线使用权
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或底层错误码
 */
    esp_err_t bsp_environment_deinit(void);

    /**
 * @brief 初始化板载 RTC，但不修改日历时间
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或底层错误码
 */
    esp_err_t bsp_rtc_init(void);

    /**
 * @brief 复制板载 RTC 当前时间和电压过低状态
 *
 * @param[out] out_snapshot RTC 快照，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化；
 *         或底层错误码
 */
    esp_err_t bsp_rtc_get_snapshot_copy(bsp_rtc_snapshot_t *out_snapshot);

    /**
 * @brief 单独读取板载 RTC 的电压过低标志
 *
 * @param[out] out_voltage_low true 表示 RTC 时间不可信，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 尚未初始化；
 *         或底层错误码
 */
    esp_err_t bsp_rtc_get_voltage_low(bool *out_voltage_low);

    /**
 * @brief 写入板载 RTC 日历时间
 *
 * @param[in] datetime 2000 至 2099 年的有效时间，仅在调用期间借用
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 日期无效；ESP_ERR_INVALID_STATE 尚未初始化；
 *         或底层错误码
 */
    esp_err_t bsp_rtc_set_datetime(const bsp_rtc_datetime_t *datetime);

    /**
 * @brief 释放板载 RTC 及其共享总线使用权
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或底层错误码
 */
    esp_err_t bsp_rtc_deinit(void);

    /** @brief 板上物理按键标识 */
    typedef enum
    {
        BSP_BUTTON_LEFT = 0,
        BSP_BUTTON_RIGHT,
        BSP_BUTTON_CONFIRM,
        BSP_BUTTON_COUNT,
    } bsp_button_id_t;

    /** @brief BSP 已识别的按键事件 */
    typedef enum
    {
        BSP_BUTTON_EVENT_PRESS = 0,
        BSP_BUTTON_EVENT_RELEASE,
        BSP_BUTTON_EVENT_CLICK,
        BSP_BUTTON_EVENT_DOUBLE_CLICK,
        BSP_BUTTON_EVENT_MULTI_CLICK,
        BSP_BUTTON_EVENT_LONG_PRESS_START,
        BSP_BUTTON_EVENT_LONG_PRESS_HOLD,
        BSP_BUTTON_EVENT_LONG_PRESS_END,
    } bsp_button_event_t;

    /**
 * @brief 板级按键事件回调
 *
 * 回调由 bsp_buttons_scan() 的调用上下文同步执行，必须快速返回，不得重入
 * bsp_buttons_* 控制 API。
 *
 * @param[in] button 物理按键
 * @param[in] event 按键事件
 * @param[in] click_count 连击次数，仅点击类事件有意义
 * @param[in] context 注册回调时传入的上下文
 */
    typedef void (*bsp_button_event_cb_t)(bsp_button_id_t button, bsp_button_event_t event,
                                          uint8_t click_count, void *context);

    /**
 * @brief 初始化三个按键的 GPIO 与同步状态机
 *
 * GPIO 仅配置为上拉输入，不启用普通 GPIO 中断。本函数不创建 Task、Queue 或 Timer。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或底层初始化错误码
 */
    esp_err_t bsp_buttons_init(void);

    /**
 * @brief 设置或清除板级按键事件回调
 *
 * 不得与 bsp_buttons_scan() 并发调用。回调和 context 的借用持续到下一次设置或
 * bsp_buttons_deinit()，以先发生者为准。
 *
 * @param[in] callback 回调；传入 NULL 表示清除
 * @param[in] context 回调上下文
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t bsp_buttons_set_event_callback_borrow(bsp_button_event_cb_t callback, void *context);

    /**
 * @brief 同步采样全部按键并推进一次去抖状态机
 *
 * 调用方负责持续调度，并传入距离上次采样实际经过的时间。事件回调在本函数调用上下文
 * 同步执行。本函数不等待、不创建执行资源。
 *
 * @param[in] elapsed_ms 距离上次采样经过的毫秒数，必须大于 0
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 时间无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化；或 Driver 错误码
 */
    esp_err_t bsp_buttons_scan(uint32_t elapsed_ms);

    /**
 * @brief 查询一个按键当前已消抖的状态
 *
 * @param[in] button 物理按键
 * @param[out] out_pressed true 表示按下
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t bsp_buttons_is_pressed(bsp_button_id_t button, bool *out_pressed);

    /**
 * @brief 释放按键 GPIO、状态机与回调借用
 *
 * 不得与 bsp_buttons_scan() 并发调用。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或底层错误码
 */
    esp_err_t bsp_buttons_deinit(void);

    /* ── 整机低功耗 ───────────────────────────────────────────────────── */

    /**
 * @brief 配置三个按键与可选内部定时器作为深睡唤醒源
 *
 * 将左、右、确认按键切换为 RTC 输入、保持上拉，并清除已有唤醒源后配置低电平 EXT1 唤醒；
 * timer_wakeup_us 大于 0 时同时启用 ESP32-S3 内部定时唤醒。调用时三个按键必须已经释放。
 * 配置成功后若后续准备失败，应调用
 * bsp_power_cancel_deep_sleep() 恢复普通 GPIO 路由。
 *
 * @param[in] timer_wakeup_us 定时唤醒间隔，单位微秒；0 表示仅允许按键唤醒
 * @return ESP_OK 配置完成；ESP_ERR_INVALID_STATE 任意按键仍处于按下状态；
 *         ESP_ERR_INVALID_ARG GPIO 不支持深睡唤醒；或底层错误码
 */
    esp_err_t bsp_power_prepare_deep_sleep(uint64_t timer_wakeup_us);

    /**
 * @brief 取消尚未进入的深睡准备并恢复三个按键的普通 GPIO 路由
 *
 * @return ESP_OK 已取消；或底层错误码
 */
    esp_err_t bsp_power_cancel_deep_sleep(void);

    /**
 * @brief 进入已经配置好按键和可选定时器唤醒源的深睡
 *
 * 本函数成功时不会返回；唤醒后芯片复位并重新执行 app_main()。
 */
    void bsp_power_start_deep_sleep(void) __attribute__((noreturn));

    /**
 * @brief 判断本次启动是否由任意按键唤醒
 *
 * @param[out] out_woken_by_button true 表示 EXT1 状态包含左、右或确认按键 GPIO
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出参数为空
 */
    esp_err_t bsp_power_was_woken_by_button(bool *out_woken_by_button);

    /**
 * @brief 判断本次启动是否由内部定时器唤醒
 *
 * @param[out] out_woken_by_timer true 表示本次由深睡定时器唤醒
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出参数为空
 */
    esp_err_t bsp_power_was_woken_by_timer(bool *out_woken_by_timer);

    /** @brief SD 卡 FATFS 在 VFS 中的固定挂载点 */
#define BSP_SD_MOUNT_POINT "/sdcard"

    /**
     * @brief SD 卡插拔 GPIO 中断回调
     *
     * 回调在 GPIO ISR 上下文执行，只能进行 FromISR 通知，不得阻塞或访问文件系统。
     *
     * @param[in] context 注册时传入的借用上下文
     */
    typedef void (*bsp_sd_detect_isr_cb_t)(void *context);

    /**
     * @brief 初始化 SD 卡槽供电、检测 GPIO 和共享 SPI2 总线
     *
     * 本函数同步完成硬件初始化，但不挂载文件系统。应在墨水屏初始化之前调用，确保共享
     * SPI2 总线包含 SD 所需的 MISO GPIO。返回成功后总线保留到 bsp_sd_deinit()。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或 GPIO、SPI 初始化错误码
     */
    esp_err_t bsp_sd_init(void);

    /**
     * @brief 设置或清除 SD 卡插拔 GPIO ISR 回调
     *
     * callback 和 context 的借用持续到下一次设置、传入 NULL 清除或 bsp_sd_deinit()。
     *
     * @param[in] callback ISR 回调；NULL 表示清除
     * @param[in] context 回调上下文
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 GPIO ISR 错误码
     */
    esp_err_t bsp_sd_set_detect_isr_callback_borrow(bsp_sd_detect_isr_cb_t callback, void *context);

    /**
     * @brief 读取卡槽物理插卡状态
     *
     * @param[out] out_present true 表示 GPIO15 为低电平且卡已插入
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化
     */
    esp_err_t bsp_sd_is_card_present(bool *out_present);

    /**
     * @brief 通过 SDSPI 同步挂载 FATFS 到 BSP_SD_MOUNT_POINT
     *
     * 不会在挂载失败时格式化卡。调用可能阻塞数秒，禁止在 ISR 或定时器回调中调用。
     *
     * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 未插卡；ESP_ERR_INVALID_STATE 状态非法；
     *         或 SDSPI、FATFS 错误码
     */
    esp_err_t bsp_sd_mount(void);

    /**
     * @brief 同步卸载 SD 卡 FATFS，保留共享 SPI2 总线供墨水屏使用
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未挂载；或 FATFS 错误码
     */
    esp_err_t bsp_sd_unmount(void);

    /**
     * @brief 释放 SD 卡槽 GPIO 和本模块拥有的 SPI2 总线
     *
     * 调用前必须清除 ISR 回调并卸载文件系统。若其他 SPI 设备仍占用总线，释放失败时保留
     * 已初始化状态供调用方继续收敛。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 状态非法；或 GPIO、SPI 错误码
     */
    esp_err_t bsp_sd_deinit(void);

    /**
     * @brief 初始化电池监测使能 GPIO、ADC 单次采样单元和校准句柄
     *
     * 初始化后监测电路保持关闭；每次读取时才临时使能。本组件不创建 Task。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或 GPIO、ADC 初始化错误码
     */
    esp_err_t bsp_battery_init(void);

    /**
     * @brief 临时使能电池监测电路并同步读取校准后的电池电压
     *
     * 函数拉高监测使能 GPIO，等待板级稳定时间，完成多次 ADC 采样与分压补偿后关闭
     * 监测电路。调用会阻塞约 200 ms，只能在 Task 上下文串行调用。
     *
     * @param[out] out_voltage_mv 电池电压，单位 mV，仅 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或 GPIO、ADC 读取错误码
     */
    esp_err_t bsp_battery_read_voltage_mv(uint32_t *out_voltage_mv);

    /**
     * @brief 释放电池 ADC 与 GPIO 资源并保持监测电路关闭
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或底层资源释放错误码
     */
    esp_err_t bsp_battery_deinit(void);

    /**
     * @brief 初始化 GPIO45 的 LEDC 蜂鸣器 PWM，初始保持静音
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；或 LEDC 配置错误码
     */
    esp_err_t bsp_buzzer_init(void);

    /**
     * @brief 以指定占空比启动蜂鸣器音调
     *
     * 本接口使用基础 LEDC 更新操作，不安装 Fade Service；调用方必须串行访问蜂鸣器 API。
     *
     * @param[in] frequency_hz 音调频率，单位 Hz，必须大于 0
     * @param[in] duty_percent PWM 高电平占空比，范围 1～50
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 频率或占空比无效；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或 LEDC 配置错误码
     */
    esp_err_t bsp_buzzer_start(uint32_t frequency_hz, uint8_t duty_percent);

    /**
     * @brief 停止蜂鸣器 PWM 并将输出保持为低电平
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 LEDC 错误码
     */
    esp_err_t bsp_buzzer_stop(void);

    /**
 * @brief 初始化 LED GPIO
 *
 * 根据 board_pin_defs.h 的引脚配置，将 LED GPIO 配置为输出模式并熄灭 LED。
 * 可重复调用，已初始化时直接返回 ESP_OK。
 *
 * @return ESP_OK 成功；或其他 GPIO 配置错误码
 */
    esp_err_t bsp_led_init(void);

    /**
 * @brief 设置 LED 点亮/熄灭状态
 *
 * @param[in] on true 点亮，false 熄灭
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t bsp_led_set_state(bool on);

    /**
 * @brief 获取 LED 当前状态
 *
 * @param[out] out_on 当前状态输出，true 为点亮
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t bsp_led_get_state(bool *out_on);

    /** @brief 板级墨水屏全局刷新模式 */
    typedef enum
    {
        BSP_EPAPER_MODE_MONOCHROME = 0,
        BSP_EPAPER_MODE_GRAYSCALE_4,
    } bsp_epaper_mode_t;

    /** @brief BSP ASCII 合成支持的可变帧格式 */
    typedef enum
    {
        BSP_EPAPER_FRAME_MONO_1BPP = 0, /**< 1 表示黑、0 表示白 */
        BSP_EPAPER_FRAME_GRAY_2BPP,     /**< 0～3 依次表示黑到白 */
    } bsp_epaper_frame_format_t;

    /** @brief 仅在同步调用期间借用的可变墨水屏帧视图 */
    typedef struct
    {
        uint8_t                  *pixels;        /**< 可写像素首地址 */
        size_t                    size_bytes;    /**< 可写缓冲区总长度 */
        size_t                    stride_bytes;  /**< 相邻两行首地址的字节间距 */
        uint16_t                  width_pixels;  /**< 帧宽度 */
        uint16_t                  height_pixels; /**< 帧高度 */
        bsp_epaper_frame_format_t format;        /**< 帧像素格式 */
    } bsp_epaper_frame_view_t;

    /** @brief 缩放 ASCII 文本的像素尺寸 */
    typedef struct
    {
        uint16_t width_pixels;  /**< 文本宽度 */
        uint16_t height_pixels; /**< 文本高度 */
    } bsp_epaper_ascii_size_t;

    /** @brief 墨水屏局部刷新矩形，横向位置和宽度必须按 8 像素对齐 */
    typedef struct
    {
        uint16_t x_pixels;      /**< 左上角横坐标 */
        uint16_t y_pixels;      /**< 左上角纵坐标 */
        uint16_t width_pixels;  /**< 区域宽度 */
        uint16_t height_pixels; /**< 区域高度 */
    } bsp_epaper_rect_t;

    /** @brief 单次 BSP ASCII 状态文本最大字符数，不含结尾空字符 */
#define BSP_EPAPER_ASCII_TEXT_MAX    32U

    /**
 * @brief 初始化 GDEY075T7 墨水屏硬件和 UC8179 驱动
 *
 * 初始化 SPI2、面板控制引脚和 UC8179 驱动，但不启动升压；控制器保持复位后的低功耗状态，
 * 直到首次全局刷新。
 *
 * @param[in] mode 单色或 4 灰阶全局刷新模式
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 GPIO、SPI、驱动错误码
 */
    esp_err_t bsp_epaper_init(bsp_epaper_mode_t mode);

    /**
     * @brief 计算紧凑 5x7 ASCII 文本的缩放后像素尺寸
     *
     * 支持大小写英文字母、数字、空格以及 `! - . : ? _`；字符之间保留一个逻辑像素间隔。
     *
     * @param[in] text 非空 ASCII 文本，最长 BSP_EPAPER_ASCII_TEXT_MAX 字符
     * @param[in] scale 5x7 字模的整数缩放倍数，必须大于 0
     * @param[out] out_size 文本像素尺寸，仅在返回 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 文本、字符、缩放或输出参数无效
     */
    esp_err_t bsp_epaper_measure_ascii_copy(const char *text, uint8_t scale,
                                            bsp_epaper_ascii_size_t *out_size);

    /**
     * @brief 在可变帧的指定左上角绘制缩放 ASCII 文本
     *
     * 本函数只把黑色字形写入调用方帧，不清空帧、不访问显示硬件，也不触发物理刷新。
     * 函数返回前结束 frame、pixels 和 text 的借用，不保存任何指针。
     *
     * @param[in,out] frame 可变 1 bpp 或 2 bpp 帧视图
     * @param[in] x_pixels 文本左上角横坐标
     * @param[in] y_pixels 文本左上角纵坐标
     * @param[in] text 非空 ASCII 文本，最长 BSP_EPAPER_ASCII_TEXT_MAX 字符
     * @param[in] scale 5x7 字模的整数缩放倍数，必须大于 0
     * @return ESP_OK 已完成帧合成；ESP_ERR_INVALID_ARG 帧、文本、字符或缩放无效；
     *         ESP_ERR_INVALID_SIZE 文本超出帧范围
     */
    esp_err_t bsp_epaper_draw_ascii_borrow(bsp_epaper_frame_view_t *frame, uint16_t x_pixels,
                                           uint16_t y_pixels, const char *text, uint8_t scale);

    /**
 * @brief 把整块墨水屏同步刷新为纯黑或纯白
 *
 * 每次调用都会完成“唤醒、全局刷新、关闭高压、深睡”的完整会话。
 *
 * @param[in] black true 全黑，false 全白
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或已经深睡；
 *         ESP_ERR_TIMEOUT BUSY 超时；或底层 SPI 错误码
 */
    esp_err_t bsp_epaper_fill(bool black);

    /**
 * @brief 使用 1 bpp 像素同步执行一次黑白全局刷新
 *
 * 每字节从 bit7 到 bit0 保存 8 个像素，1 表示黑、0 表示白。函数返回前结束输入
 * 缓冲区借用，并关闭面板高压、使控制器进入深睡。
 *
 * @param[in] pixels_1bpp 800x480、1 bpp 黑白帧，仅在调用期间借用
 * @param[in] size_bytes 缓冲区长度，必须为 48000 字节
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 初始化模式错误或已经深睡；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 SPI 错误码
 */
    esp_err_t bsp_epaper_display_monochrome_borrow(const uint8_t *pixels_1bpp, size_t size_bytes);

    /**
 * @brief 使用 2 bpp 像素同步执行一次全局刷新
 *
 * 每字节从高位到低位保存 4 个像素，灰阶值 0、1、2、3 分别表示黑、灰阶 1、
 * 灰阶 2、白。当前黑白效果测试会在驱动内把 0、1 合并为黑，2、3 合并为白，并使用
 * 黑白全刷波形；下发格式保持不变。函数返回前结束输入缓冲区借用，并关闭面板高压、
 * 使控制器进入深睡。
 *
 * @param[in] pixels_2bpp 800x480、2 bpp 灰阶帧，仅在调用期间借用
 * @param[in] size_bytes 缓冲区长度，必须为 96000 字节
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数无效；
 *         ESP_ERR_INVALID_STATE 初始化模式错误或已经深睡；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 SPI 错误码
 */
    esp_err_t bsp_epaper_display_grayscale_borrow(const uint8_t *pixels_2bpp, size_t size_bytes);

    /**
 * @brief 把完整 2 bpp 前后帧按中间阈值转换为黑白并刷新多个局部窗口
 *
 * 本函数在一次有界会话中完成“唤醒、恢复 previous RAM、逐窗口刷新、关闭高压、深睡”。
 * previous 和 current 每字节从高位到低位保存四个灰度像素，0、1 映射为黑，2、3
 * 映射为白。函数返回前结束所有输入借用。
 *
 * @param[in] previous_pixels_2bpp 上一次确认成功显示的完整 2 bpp 帧
 * @param[in] current_pixels_2bpp 待显示的完整 2 bpp 帧
 * @param[in] size_bytes 每帧长度，必须为 96000 字节
 * @param[in] rects 互不重叠的局刷矩形数组
 * @param[in] rect_count 矩形数，必须大于 0；BSP 不设置固定上限
 * @return ESP_OK 全部窗口刷新成功；ESP_ERR_INVALID_ARG 参数或矩形无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化、模式错误或已经深睡；
 *         ESP_ERR_TIMEOUT BUSY 超时；或底层 SPI、驱动错误码
 */
    esp_err_t bsp_epaper_display_partial_monochrome_borrow(const uint8_t *previous_pixels_2bpp,
                                                           const uint8_t *current_pixels_2bpp,
                                                           size_t         size_bytes,
                                                           const bsp_epaper_rect_t *rects,
                                                           size_t                   rect_count);

    /**
 * @brief 结束墨水屏生命周期并确保高压关闭、控制器深睡
 *
 * 正常情况下每次全刷结束后控制器已经深睡；本函数还会处理异常刷新后仍处于唤醒状态的
 * 情况。调用后必须释放并重新初始化，才能进行下一次刷新。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或已经深睡；
 *         ESP_ERR_TIMEOUT BUSY 超时；或底层 SPI 错误码
 */
    esp_err_t bsp_epaper_sleep(void);

    /**
 * @brief 释放墨水屏驱动、SPI 总线和控制 GPIO
 *
 * 必须先成功调用 bsp_epaper_sleep()。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或面板尚未深睡；
 *         或底层资源释放错误码
 */
    esp_err_t bsp_epaper_deinit(void);

#ifdef __cplusplus
}
#endif
