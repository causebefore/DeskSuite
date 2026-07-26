/**
 * @file device_display.h
 * @brief 与面板型号、帧布局和 GPIO 无关的同步显示能力
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 设备显示全局刷新模式 */
    typedef enum
    {
        DEVICE_DISPLAY_MODE_MONOCHROME = 0,
        DEVICE_DISPLAY_MODE_GRAYSCALE_4,
    } device_display_mode_t;

    /** @brief 与具体帧编码无关的四级显示色调 */
    typedef enum
    {
        DEVICE_DISPLAY_TONE_BLACK = 0,
        DEVICE_DISPLAY_TONE_DARK_GRAY,
        DEVICE_DISPLAY_TONE_LIGHT_GRAY,
        DEVICE_DISPLAY_TONE_WHITE,
    } device_display_tone_t;

    /** @brief device_display_blit_borrow() 支持的源图像像素格式 */
    typedef enum
    {
        DEVICE_DISPLAY_PIXEL_FORMAT_MONO_1BPP = 0,
        DEVICE_DISPLAY_PIXEL_FORMAT_GRAY_2BPP,
        DEVICE_DISPLAY_PIXEL_FORMAT_GRAY_8BPP,
    } device_display_pixel_format_t;

    /** @brief 只在同步调用期间借用的只读源图像视图 */
    typedef struct
    {
        const void                   *pixels;        /**< 源像素首地址 */
        size_t                        size_bytes;    /**< 可读取的源数据总长度 */
        size_t                        stride_bytes;  /**< 相邻两行首地址的字节间距 */
        uint16_t                      width_pixels;  /**< 源图像宽度 */
        uint16_t                      height_pixels; /**< 源图像高度 */
        device_display_pixel_format_t pixel_format;  /**< 源像素格式 */
    } device_display_image_view_t;

    /** @brief 设备显示能力快照，不包含内部帧或驱动句柄 */
    typedef struct
    {
        uint16_t              width_pixels;  /**< 可显示宽度 */
        uint16_t              height_pixels; /**< 可显示高度 */
        device_display_mode_t mode;          /**< 当前全局刷新模式 */
    } device_display_info_t;

    /** @brief 缩放 ASCII 文本的像素尺寸 */
    typedef struct
    {
        uint16_t width_pixels;  /**< 文本宽度 */
        uint16_t height_pixels; /**< 文本高度 */
    } device_display_ascii_size_t;

    /**
 * @brief 初始化显示硬件并创建组件内部单帧
 *
 * 单色模式内部帧占 48 KB，4 灰阶模式内部帧占 96 KB；优先从 PSRAM 分配，失败时
 * 回退到普通 8 位内存。内部帧默认清为白色。本组件不创建 Task，所有 API 均不得并发调用。
 * 初始化只准备硬件和帧缓冲区，面板在两次刷新之间保持关闭高压的低功耗状态。
 *
 * @param[in] mode 单色或 4 灰阶全局刷新模式，初始化成功后固定到 deinit
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 帧分配失败；ESP_ERR_INVALID_STATE 已初始化；
 *         ESP_ERR_TIMEOUT 面板 BUSY 超时；或底层错误码
 */
    esp_err_t device_display_init(device_display_mode_t mode);

    /**
 * @brief 复制当前显示尺寸和刷新模式
 *
 * @param[out] out_info 显示能力快照，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t device_display_get_info_copy(device_display_info_t *out_info);

    /**
 * @brief 把内部帧清为指定色调但不刷新面板
 *
 * 单色模式会把黑、深灰映射为黑，把浅灰、白映射为白。
 *
 * @param[in] tone 目标色调
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 色调无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化或已经深睡
 */
    esp_err_t device_display_clear(device_display_tone_t tone);

    /**
 * @brief 在内部帧中填充一个矩形区域但不刷新面板
 *
 * 区域必须完整位于显示范围内。单色模式的色调映射规则与 device_display_clear() 相同。
 *
 * @param[in] x_pixels 区域左上角横坐标
 * @param[in] y_pixels 区域左上角纵坐标
 * @param[in] width_pixels 区域宽度，必须大于 0
 * @param[in] height_pixels 区域高度，必须大于 0
 * @param[in] tone 填充色调
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数或区域无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化或已经深睡
 */
    esp_err_t device_display_fill_rect(uint16_t x_pixels, uint16_t y_pixels, uint16_t width_pixels,
                                       uint16_t height_pixels, device_display_tone_t tone);

    /**
 * @brief 把源图像同步转换并复制到内部帧但不刷新面板
 *
 * MONO_1BPP 每字节从 bit7 到 bit0 保存 8 个像素，1 表示黑；GRAY_2BPP 每字节从
 * bits[7:6] 到 bits[1:0] 保存 4 个像素，0、1、2、3 依次表示黑到白；GRAY_8BPP
 * 每像素使用 0～255 表示黑到白。目标区域必须完整位于显示范围内。函数返回前结束
 * image 和 pixels 的借用，不保存内部指针。
 *
 * @param[in] x_pixels 目标左上角横坐标
 * @param[in] y_pixels 目标左上角纵坐标
 * @param[in] image 源图像视图，仅在调用期间借用
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 图像、步长、长度或目标区域无效；
 *         ESP_ERR_INVALID_STATE 尚未初始化或已经深睡
 */
    esp_err_t device_display_blit_borrow(uint16_t x_pixels, uint16_t y_pixels,
                                         const device_display_image_view_t *image);

    /**
     * @brief 计算紧凑 5x7 ASCII 文本的缩放后像素尺寸
     *
     * 支持大小写英文字母、数字、空格以及 `! - . : ? _`，单次最多 32 个字符。
     *
     * @param[in] text 非空 ASCII 文本
     * @param[in] scale 字模整数缩放倍数，必须大于 0
     * @param[out] out_size 文本像素尺寸，仅在返回 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 文本、字符、缩放或输出参数无效
     */
    esp_err_t device_display_measure_ascii_copy(const char *text, uint8_t scale,
                                                device_display_ascii_size_t *out_size);

    /**
     * @brief 在内部帧的指定左上角绘制缩放 ASCII 文本但不刷新面板
     *
     * 只写入黑色字形，不清空已有帧内容、不自动换行。函数返回前结束 text 借用。
     *
     * @param[in] x_pixels 文本左上角横坐标
     * @param[in] y_pixels 文本左上角纵坐标
     * @param[in] text 非空 ASCII 文本
     * @param[in] scale 字模整数缩放倍数，必须大于 0
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 文本、字符或缩放无效；
     *         ESP_ERR_INVALID_SIZE 文本超出屏幕；ESP_ERR_INVALID_STATE 尚未初始化或已深睡
     */
    esp_err_t device_display_draw_ascii_borrow(uint16_t x_pixels, uint16_t y_pixels,
                                               const char *text, uint8_t scale);

    /**
     * @brief 清白内部帧并同步居中呈现缩放 ASCII 状态文本
     *
     * 使用 BSP 的紧凑 5x7 字模合成能力，支持大小写英文字母、数字、空格及
     * `! - . : ? _`。函数返回前结束 text 借用，并完成一次最长约 15 秒的物理全局刷新。
     * 所有显示 API 仍不得并发调用。
     *
     * @param[in] text 非空 ASCII 状态文本
     * @param[in] scale 字模整数缩放倍数，必须大于 0
     * @return ESP_OK 呈现成功；ESP_ERR_INVALID_ARG 文本、字符或缩放无效；
     *         ESP_ERR_INVALID_SIZE 文本超出屏幕；ESP_ERR_INVALID_STATE 尚未初始化或已深睡；
     *         ESP_ERR_TIMEOUT 面板 BUSY 超时；或底层错误码
     */
    esp_err_t device_display_present_ascii_centered_borrow(const char *text, uint8_t scale);

    /**
 * @brief 使用内部帧同步执行一次全局刷新
 *
 * 4 灰阶模式会在线生成并发送两张 UC8179 位平面，不额外分配完整位平面；单色模式
 * 直接提交 1 bpp 内部帧。每次调用均执行“唤醒、全局刷新、关闭高压、深睡”，函数返回时
 * 刷新和深睡已经完成，最长阻塞约 15 秒。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或已经深睡；
 *         ESP_ERR_TIMEOUT 面板 BUSY 超时；或底层错误码
 */
    esp_err_t device_display_present(void);

    /**
 * @brief 结束显示生命周期并确保高压关闭、控制器深睡
 *
 * 正常全刷结束后控制器已经深睡，本函数负责封闭生命周期；内部帧仍由组件持有，但不能
 * 继续修改或刷新，需调用 device_display_deinit() 释放。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或已经深睡；
 *         ESP_ERR_TIMEOUT 面板 BUSY 超时；或底层错误码
 */
    esp_err_t device_display_sleep(void);

    /**
 * @brief 释放处于深睡状态的显示硬件和内部帧
 *
 * 仅在 BSP 资源成功释放后才释放内部帧并恢复未初始化状态。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化或尚未深睡；或底层错误码
 */
    esp_err_t device_display_deinit(void);

#ifdef __cplusplus
}
#endif
