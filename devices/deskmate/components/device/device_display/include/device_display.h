/**
 * @file device_display.h
 * @brief 与面板型号、SPI 和 Board 参数无关的显示设备能力
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 显示设备能力快照 */
    typedef struct
    {
        uint16_t width_pixels;
        uint16_t height_pixels;
    } device_display_info_t;

    /** @brief 初始化显示设备、帧缓冲和传输资源 */
    esp_err_t device_display_init(void);

    /**
     * @brief 同步停止新帧并等待显示传输静止
     *
     * 本函数保留显示 Task、总线、控制器和缓冲区，适用于 Light-sleep 前的可逆停止。
     *
     * @param[in] timeout_ms 等待在途刷新完成的总超时，单位毫秒
     * @return ESP_OK 已停止或原本已停止；ESP_ERR_INVALID_ARG 超时无效；
     *         ESP_ERR_INVALID_STATE 尚未初始化；或底层错误码
     */
    esp_err_t device_display_stop(uint32_t timeout_ms);

    /**
     * @brief 恢复已停止的显示设备
     *
     * 本函数不重新初始化总线或面板控制器。
     *
     * @return ESP_OK 已运行或原本正在运行；ESP_ERR_INVALID_STATE 尚未初始化；
     *         或底层错误码
     */
    esp_err_t device_display_start(void);

    /** @brief 等待传输完成并释放显示设备资源 */
    esp_err_t device_display_deinit(void);

    /** @brief 复制显示设备能力 */
    esp_err_t device_display_get_info_copy(device_display_info_t *out_info);

    /** @brief 把 1 bpp 区域同步写入设备帧缓冲 */
    esp_err_t device_display_write_i1_area(int x1, int y1, int x2, int y2, const uint8_t *pixels,
                                           uint32_t stride_bytes);

    /** @brief 异步提交一次显示刷新 */
    esp_err_t device_display_flush_async(void);

    /** @brief 有界等待已经提交的显示刷新完成 */
    esp_err_t device_display_wait_flush_done(uint32_t timeout_ms);

    /** @brief 读取最近一秒显示刷新帧率 */
    uint32_t device_display_get_flush_fps(void);

    /** @brief 读取启动后的显示刷新总次数 */
    uint32_t device_display_get_total_flush_count(void);

#ifdef __cplusplus
}
#endif
