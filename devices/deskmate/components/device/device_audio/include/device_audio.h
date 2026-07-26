/**
 * @file device_audio.h
 * @brief 与 I2S、Codec 型号和 Board 参数无关的音频设备能力
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

    /** @brief 与具体 Codec、I2S 和 Board 无关的音频能力配置 */
    typedef struct
    {
        uint32_t sample_rate_hz; /**< 输入输出共用采样率，单位 Hz */
        uint8_t  initial_volume; /**< 初始输出音量，范围 0～100 */
        uint8_t  input_gain_db;  /**< 输入增益，单位 dB */
    } device_audio_config_t;

    /**
     * @brief 初始化音频输入、输出和 Codec 资源
     * @param[in] config 音频能力配置，仅在调用期间借用
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法；ESP_ERR_INVALID_STATE 已初始化；
     *         或 BSP 初始化错误
     */
    esp_err_t device_audio_init(const device_audio_config_t *config);

    /** @brief 释放音频输入、输出和 Codec 资源 */
    esp_err_t device_audio_deinit(void);

    /** @brief 使能或关闭扬声器输出 */
    esp_err_t device_audio_enable_output(bool enable);

    /** @brief 使能或关闭麦克风输入 */
    esp_err_t device_audio_enable_input(bool enable);

    /** @brief 设置 0～100 的扬声器输出音量 */
    esp_err_t device_audio_set_output_volume(int volume);

    /**
     * @brief 同步写入 16-bit PCM 单声道样本
     * @param[in] data PCM 样本，仅在调用期间借用
     * @param[in] sample_count 请求写入样本数
     * @param[out] out_written 实际写入样本数，仅在 ESP_OK 时有效
     */
    esp_err_t device_audio_write(const int16_t *data, size_t sample_count, size_t *out_written);

    /**
     * @brief 同步读取 16-bit PCM 样本
     * @param[out] dest PCM 输出缓冲区
     * @param[in] sample_count 请求读取样本数
     * @param[out] out_read 实际读取样本数，仅在 ESP_OK 时有效
     */
    esp_err_t device_audio_read(int16_t *dest, size_t sample_count, size_t *out_read);

    /** @brief 返回当前设备音频输出采样率 */
    uint32_t device_audio_get_sample_rate_hz(void);

#ifdef __cplusplus
}
#endif
