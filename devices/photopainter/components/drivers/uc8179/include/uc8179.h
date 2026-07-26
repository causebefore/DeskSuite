/**
 * @file uc8179.h
 * @brief UC8179 墨水屏控制器驱动接口
 *
 * 驱动只实现控制器协议、初始化时序和同步刷新流程，不直接访问 GPIO、SPI 或板级定义。
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

    /** @brief UC8179 支持的最大水平像素数 */
#define UC8179_MAX_WIDTH_PIXELS  800U

    /** @brief UC8179 支持的最大垂直像素数 */
#define UC8179_MAX_HEIGHT_PIXELS 600U

    /** @brief UC8179 全局刷新模式 */
    typedef enum
    {
        UC8179_MODE_MONOCHROME = 0,
        UC8179_MODE_GRAYSCALE_4,
    } uc8179_mode_t;

    /** @brief UC8179 局部刷新矩形，横向位置和宽度必须按 8 像素对齐 */
    typedef struct
    {
        uint16_t x_pixels;      /**< 左上角横坐标 */
        uint16_t y_pixels;      /**< 左上角纵坐标 */
        uint16_t width_pixels;  /**< 区域宽度 */
        uint16_t height_pixels; /**< 区域高度 */
    } uc8179_rect_t;

    /**
 * @brief 同步写入一段命令或数据
 *
 * 回调返回前必须完成传输，不得保存输入缓冲区。控制器要求 MSB first、SPI Mode 0；
 * is_data 为 false 时传输命令，为 true 时传输参数或图像数据。
 *
 * @param[in] is_data true 表示数据，false 表示命令
 * @param[in] data 待发送数据，仅在回调期间有效
 * @param[in] size_bytes 数据长度
 * @param[in] context 配置时传入的 I/O 上下文
 * @return ESP_OK 成功，或底层传输错误码
 */
    typedef esp_err_t (*uc8179_write_cb_t)(bool is_data, const uint8_t *data, size_t size_bytes,
                                           void *context);

    /**
 * @brief 设置硬件复位引脚电平
 *
 * @param[in] high true 输出高电平，false 输出低电平
 * @param[in] context 配置时传入的 I/O 上下文
 * @return ESP_OK 成功，或底层 GPIO 错误码
 */
    typedef esp_err_t (*uc8179_set_reset_cb_t)(bool high, void *context);

    /**
 * @brief 读取控制器忙状态
 *
 * 回调负责把板级有效电平转换为逻辑忙状态。
 *
 * @param[out] out_busy true 表示控制器正忙
 * @param[in] context 配置时传入的 I/O 上下文
 * @return ESP_OK 成功，或底层 GPIO 错误码
 */
    typedef esp_err_t (*uc8179_read_busy_cb_t)(bool *out_busy, void *context);

    /**
 * @brief 阻塞等待指定毫秒数
 *
 * @param[in] delay_ms 等待时间，单位毫秒
 * @param[in] context 配置时传入的 I/O 上下文
 */
    typedef void (*uc8179_delay_cb_t)(uint32_t delay_ms, void *context);

    /** @brief UC8179 控制器与面板初始化配置 */
    typedef struct
    {
        uint16_t              width_pixels;          /**< 面板水平像素数，必须为 8 的倍数 */
        uint16_t              height_pixels;         /**< 面板垂直像素数 */
        uint32_t              busy_timeout_ms;       /**< 单次 BUSY 等待超时 */
        uint8_t               power_setting[4];      /**< PWR(0x01) 的面板电源参数 */
        uint8_t               booster_soft_start[4]; /**< BTST(0x06) 的升压软启动参数 */
        uint8_t               grayscale_booster_soft_start[4]; /**< 4 灰阶 BTST 参数 */
        uint8_t               panel_setting;                   /**< PSR(0x00) 参数 */
        uint8_t               vcom_data_interval[2];           /**< CDI(0x50) 参数 */
        uint8_t               tcon_setting;                    /**< TCON(0x60) 参数 */
        uint8_t               grayscale_temperature;           /**< OTP 灰阶波形的固定温度参数 */
        uint8_t               partial_temperature;             /**< OTP 黑白局刷固定温度参数 */
        uint8_t               partial_vcom_data_interval[2];   /**< 局刷 CDI(0x50) 参数 */
        uc8179_write_cb_t     write;
        uc8179_set_reset_cb_t set_reset;
        uc8179_read_busy_cb_t read_busy;
        uc8179_delay_cb_t     delay;
        void                 *io_context;
    } uc8179_config_t;

    /**
 * @brief 可由调用方静态分配的 UC8179 实例
 *
 * 字段属于驱动内部状态；调用方只负责实例存储期，不应直接修改字段。
 */
    typedef struct
    {
        uc8179_config_t config;
        uc8179_mode_t   mode;
        bool            initialized;
        bool            powered_on;
    } uc8179_t;

    /**
 * @brief 初始化驱动实例并复制配置
 *
 * 本函数不访问硬件；调用 uc8179_power_on() 后才执行复位和面板初始化。
 *
 * @param[out] out_controller 调用方提供且已清零的实例存储
 * @param[in] config 配置，仅在调用期间借用
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 配置无效；ESP_ERR_INVALID_STATE 已初始化
 */
    esp_err_t uc8179_init(uc8179_t *out_controller, const uc8179_config_t *config);

    /**
 * @brief 硬复位控制器并按指定全刷模式初始化
 *
 * 本函数同步等待电源稳定，最长阻塞约 busy_timeout_ms 加固定复位延时。
 *
 * @param[in,out] controller 已初始化实例
 * @param[in] mode 单色或 4 灰阶全局刷新模式
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 状态非法；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 I/O 错误码
 */
    esp_err_t uc8179_power_on(uc8179_t *controller, uc8179_mode_t mode);

    /**
 * @brief 使用 OTP 波形把整屏刷新为纯黑或纯白
 *
 * 发送两帧 1 bpp 数据并同步等待全刷完成，最长阻塞约 busy_timeout_ms。
 *
 * @param[in,out] controller 已上电实例
 * @param[in] black true 全黑，false 全白
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 状态非法；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 I/O 错误码
 */
    esp_err_t uc8179_fill(uc8179_t *controller, bool black);

    /**
 * @brief 使用 1 bpp 帧同步执行一次黑白全局刷新
 *
 * 输入按行连续排列，每字节从 bit7 到 bit0 保存 8 个像素，1 表示黑、0 表示白。
 * 函数返回前完成数据借用。
 *
 * @param[in,out] controller 已按 UC8179_MODE_MONOCHROME 上电的实例
 * @param[in] pixels_1bpp 黑白像素，仅在调用期间借用
 * @param[in] size_bytes 缓冲区长度，必须严格等于 width * height / 8
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 缓冲区或长度无效；
 *         ESP_ERR_INVALID_STATE 状态或刷新模式非法；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 I/O 错误码
 */
    esp_err_t uc8179_display_monochrome_borrow(uc8179_t *controller, const uint8_t *pixels_1bpp,
                                               size_t size_bytes);

    /**
 * @brief 使用 2 bpp 输入同步执行一次 4 灰阶或阈值黑白全局刷新
 *
 * 输入按行连续排列，每字节从高位到低位保存 4 个像素：bits[7:6] 为第一个像素，
 * bits[5:4]、bits[3:2]、bits[1:0] 依次为后续像素。像素值 0、1、2、3
 * 分别选择黑、灰阶 1、灰阶 2、白。控制器按 UC8179_MODE_GRAYSCALE_4 上电时使用
 * OTP 四灰阶波形；按 UC8179_MODE_MONOCHROME 上电时把 0、1 合并为黑，2、3 合并为白，
 * 并使用黑白波形。函数返回前完成数据借用。
 *
 * @param[in,out] controller 已按 UC8179_MODE_GRAYSCALE_4 或 UC8179_MODE_MONOCHROME 上电的实例
 * @param[in] pixels_2bpp 2 bpp 灰阶像素，仅在调用期间借用
 * @param[in] size_bytes 缓冲区长度，必须严格等于 width * height / 4
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 缓冲区或长度无效；
 *         ESP_ERR_INVALID_STATE 状态或刷新模式非法；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 I/O 错误码
 */
    esp_err_t uc8179_display_grayscale_borrow(uc8179_t *controller, const uint8_t *pixels_2bpp,
                                              size_t size_bytes);

    /**
 * @brief 把完整 2 bpp 前后帧在线转为黑白并同步刷新多个局部窗口
 *
 * 函数先把 previous_pixels_2bpp 转换后恢复到控制器 previous RAM，再按 rects 顺序
 * 对 current_pixels_2bpp 的各窗口分别执行一次局刷。每个窗口都会等待 BUSY 完成；
 * 函数不负责关闭高压或深睡，返回前结束所有输入借用。
 *
 * @param[in,out] controller 已按 UC8179_MODE_MONOCHROME 上电的实例
 * @param[in] previous_pixels_2bpp 上一次成功显示的完整 2 bpp 帧
 * @param[in] current_pixels_2bpp 待显示的完整 2 bpp 帧
 * @param[in] size_bytes 每帧长度，必须严格等于 width * height / 4
 * @param[in] rects 互不重叠的局刷矩形数组，X 和宽度必须按 8 像素对齐
 * @param[in] rect_count 矩形数，必须大于 0；Driver 不设置固定上限
 * @return ESP_OK 全部窗口刷新成功；ESP_ERR_INVALID_ARG 帧、矩形或长度无效；
 *         ESP_ERR_INVALID_STATE 控制器状态或模式非法；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 I/O 错误码
 */
    esp_err_t uc8179_display_partial_from_gray2_borrow(uc8179_t            *controller,
                                                       const uint8_t       *previous_pixels_2bpp,
                                                       const uint8_t       *current_pixels_2bpp,
                                                       size_t               size_bytes,
                                                       const uc8179_rect_t *rects,
                                                       size_t               rect_count);

    /**
 * @brief 关闭高压并进入深睡
 *
 * 深睡后只能通过硬件复位恢复；可先调用 uc8179_deinit()，再重新初始化和上电。
 *
 * @param[in,out] controller 已上电实例
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 状态非法；ESP_ERR_TIMEOUT BUSY 超时；
 *         或底层 I/O 错误码
 */
    esp_err_t uc8179_deep_sleep(uc8179_t *controller);

    /**
 * @brief 清除驱动配置并恢复未初始化状态
 *
 * 必须先成功进入深睡或尚未执行上电。
 *
 * @param[in,out] controller 已初始化且未上电实例
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；ESP_ERR_INVALID_STATE 状态非法
 */
    esp_err_t uc8179_deinit(uc8179_t *controller);

#ifdef __cplusplus
}
#endif
