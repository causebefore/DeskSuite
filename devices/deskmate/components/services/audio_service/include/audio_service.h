#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 初始化音频服务自己的状态与互斥资源
     *
     * 调用前必须已由 Composition Root 初始化 device_audio。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 已初始化或 Device 尚未初始化
     */
    esp_err_t audio_service_init(void);

    /**
     * @brief 停止输入输出并释放音频服务自己的互斥资源
     *
     * 本函数不释放 device_audio，其生命周期由 Composition Root 管理。
     *
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或链路停止错误
     */
    esp_err_t audio_service_deinit(void);

    /**
     * @brief 独立使能或关闭输入链路
     * @param[in] enable true 使能，false 关闭
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 Device 错误
     */
    esp_err_t audio_service_enable_input(bool enable);

    /**
     * @brief 独立使能或关闭输出链路
     * @param[in] enable true 使能，false 关闭
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 Device 错误
     */
    esp_err_t audio_service_enable_output(bool enable);

    /**
     * @brief 停止录音和播放，但保留已初始化资源
     * @return ESP_OK 成功；或输入/输出停止错误
     */
    esp_err_t audio_service_stop(void);

    /**
     * @brief 同步播放 16-bit PCM 单声道样本
     * @param[in] data PCM 样本，仅在调用期间借用
     * @param[in] sample_count 请求写入样本数
     * @param[out] out_written 实际写入样本数，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 输出未运行；或 Device 写入错误
     */
    esp_err_t audio_service_write(const int16_t *data, size_t sample_count, size_t *out_written);

    /**
     * @brief 同步读取 16-bit PCM 样本
     * @param[out] dest PCM 输出缓冲区
     * @param[in] sample_count 请求读取样本数
     * @param[out] out_read 实际读取样本数，仅在 ESP_OK 时有效
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 输入未运行；或 Device 读取错误
     */
    esp_err_t audio_service_read(int16_t *dest, size_t sample_count, size_t *out_read);

    /**
     * @brief 设置播放音量
     * @param[in] volume 音量值，范围 0～100
     * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 超出范围；或 Device 错误
     */
    esp_err_t audio_service_set_volume(int volume);

    /**
     * @brief 设置静音状态
     * @param[in] muted true 静音，false 取消静音
     * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未初始化；或 Device 错误
     */
    esp_err_t audio_service_set_muted(bool muted);

    /**
     * @brief 查询静音状态
     *
     * @return true 静音，false 未静音
     */
    bool audio_service_is_muted(void);

    /**
     * @brief 查询当前音量
     *
     * @return 当前音量值 (0-100)
     */
    int audio_service_get_volume(void);

    /**
     * @brief 查询音频服务是否运行中
     *
     * @return true 运行中，false 已停止
     */
    bool audio_service_is_running(void);

    /** @brief 查询音频服务是否已经初始化。 */
    bool audio_service_is_initialized(void);

    /**
     * @brief 查询当前 Device 音频采样率
     * @return 已初始化时返回采样率；否则返回 0
     */
    uint32_t audio_service_get_sample_rate_hz(void);

#ifdef __cplusplus
}
#endif
