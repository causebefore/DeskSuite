#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief 音频 Service 可逆运行状态。 */
    typedef enum
    {
        AUDIO_SERVICE_STATE_UNINITIALIZED = 0, /*!< 尚未初始化 */
        AUDIO_SERVICE_STATE_STOPPED,           /*!< 固定资源保留，播放 Task 不存在 */
        AUDIO_SERVICE_STATE_RUNNING,           /*!< 接受文件与 PCM 播放事务 */
        AUDIO_SERVICE_STATE_STOPPING,          /*!< 正在取消事务并等待 Task 退出 */
        AUDIO_SERVICE_STATE_CLEANUP_FAILED,    /*!< 停止不完整，只允许继续 stop 收敛 */
    } audio_service_state_t;

    /** @brief 当前唯一输出事务状态。 */
    typedef enum
    {
        AUDIO_SERVICE_PLAYBACK_STATE_IDLE = 0,     /*!< 无活动或待处理输出 */
        AUDIO_SERVICE_PLAYBACK_STATE_FILE_PENDING, /*!< 文件等待 PCM 流关闭 */
        AUDIO_SERVICE_PLAYBACK_STATE_FILE,         /*!< 正在解码并播放文件 */
        AUDIO_SERVICE_PLAYBACK_STATE_PCM_STREAM,   /*!< 正在消费调用方 PCM 流 */
        AUDIO_SERVICE_PLAYBACK_STATE_DRAINING,     /*!< 正在排空 PCM 缓冲 */
        AUDIO_SERVICE_PLAYBACK_STATE_CANCELLING,   /*!< 正在丢弃或取消输出 */
    } audio_service_playback_state_t;

    /** @brief PCM 流格式；样本固定为有符号 16-bit 小端交错布局。 */
    typedef struct
    {
        uint32_t sample_rate_hz; /*!< 输入采样率 */
        uint8_t  channel_count;  /*!< 输入声道数，仅支持 1 或 2 */
    } audio_service_pcm_stream_config_t;

    /** @brief 文件播放终态。 */
    typedef enum
    {
        AUDIO_SERVICE_FILE_PLAYBACK_RESULT_COMPLETED = 0, /*!< 已完整播放或达到时长上限 */
        AUDIO_SERVICE_FILE_PLAYBACK_RESULT_CANCELLED,     /*!< 被显式取消或 PCM 流抢占 */
        AUDIO_SERVICE_FILE_PLAYBACK_RESULT_FAILED,        /*!< 文件、解码、转换或输出失败 */
    } audio_service_file_playback_result_state_t;

    /** @brief 文件播放终态事件负载。 */
    typedef struct
    {
        uint64_t                                   request_id; /*!< 原始非零请求 ID */
        audio_service_file_playback_result_state_t state;      /*!< 终态 */
        esp_err_t                                  error;      /*!< 失败错误；其余终态为 ESP_OK */
    } audio_service_file_playback_result_t;

    /** @brief Audio Service 事件。 */
    typedef enum
    {
        AUDIO_SERVICE_EVENT_FILE_PLAYBACK_FINISHED = 0, /*!< 文件播放进入唯一终态 */
    } audio_service_event_t;

    ESP_EVENT_DECLARE_BASE(AUDIO_SERVICE_EVENT);

    /** @brief 音频 Service 的有界运行摘要。 */
    typedef struct
    {
        audio_service_state_t          state;              /*!< 生命周期状态 */
        audio_service_playback_state_t playback_state;     /*!< 输出事务状态 */
        bool                           output_active;      /*!< 扬声器硬件链路是否开启 */
        bool                           task_created;       /*!< 播放 Task 是否存在 */
        bool                           task_parked;        /*!< Task 是否在无期限等待 */
        uint64_t                       active_request_id;  /*!< 当前文件请求；无则为 0 */
        uint64_t                       pending_request_id; /*!< 等待 PCM 结束的文件请求；无则为 0 */
        uint64_t                       active_stream_id;   /*!< 当前 PCM 流；无则为 0 */
        esp_err_t                      last_error;         /*!< 最近生命周期或播放错误 */
    } audio_service_status_t;

    /**
 * @brief 初始化唯一输出 Runtime 的固定资源
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 重复初始化或 Device 未初始化；其他值表示资源错误
 */
    esp_err_t audio_service_init(void);

    /**
 * @brief 创建唯一播放 Task 并开始接受输出事务
 * @return ESP_OK 已运行；ESP_ERR_INVALID_STATE 生命周期不允许；ESP_ERR_NO_MEM 创建失败
 */
    esp_err_t audio_service_start(void);

    /**
 * @brief 取消输出事务并有界等待播放 Task 协作退出
 * @param[in] timeout_ms 最长等待时间，必须大于 0
 * @return ESP_OK 已停止；ESP_ERR_TIMEOUT Task 未及时退出；其他值表示生命周期或输出错误
 */
    esp_err_t audio_service_stop(uint32_t timeout_ms);

    /**
 * @brief 从 STOPPED 释放固定资源
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 尚未停止或仍有 Task
 */
    esp_err_t audio_service_deinit(void);

    /**
 * @brief 复制完整运行摘要
 * @param[out] out_status 状态输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
    esp_err_t audio_service_get_status_copy(audio_service_status_t *out_status);

    /**
 * @brief 异步请求播放一个 MP3 文件
 *
 * 返回前复制路径。同一非零请求 ID 会合并；已有其他文件请求时明确拒绝。
 *
 * @param[in] path 文件绝对路径
 * @param[in] request_id 非零请求 ID
 * @return ESP_OK 已接受或已合并；ESP_ERR_INVALID_ARG 参数无效；ESP_ERR_INVALID_STATE 无可用槽
 */
    esp_err_t audio_service_request_play_mp3_file_copy(const char *path, uint64_t request_id);

    /**
 * @brief 异步取消匹配的文件播放请求
 * @param[in] request_id 非零请求 ID
 * @return ESP_OK 已记录；ESP_ERR_NOT_FOUND 当前无匹配请求；其他值表示生命周期错误
 */
    esp_err_t audio_service_request_cancel_file_playback(uint64_t request_id);

    /**
 * @brief 同步打开高优先级 PCM 流
 *
 * 必要时取消正在播放的 MP3。返回 ESP_OK 时流已成为唯一活动 PCM 流。
 *
 * @param[in] config 输入格式，仅在调用期间借用
 * @param[in] timeout_ms 等待抢占完成的最长时间
 * @param[out] out_stream_id 新流的非零 ID
 * @return ESP_OK 已打开；ESP_ERR_INVALID_STATE 已有 PCM 流；ESP_ERR_TIMEOUT 未完成抢占
 */
    esp_err_t audio_service_open_pcm_stream(const audio_service_pcm_stream_config_t *config, uint32_t timeout_ms,
                                            uint64_t *out_stream_id);

    /**
 * @brief 把 PCM 样本复制到活动流缓冲区
 * @param[in] stream_id `open_pcm_stream` 返回的流 ID
 * @param[in] samples 大型 PCM 缓冲，仅在调用期间借用
 * @param[in] sample_count 交错样本值数量，不是帧数
 * @param[in] timeout_ms 等待缓冲空间的最长时间
 * @param[out] out_written 实际复制的样本值数量，超时时可能小于请求值
 * @return ESP_OK 已完整复制；ESP_ERR_TIMEOUT 仅部分复制；其他值表示参数或生命周期错误
 */
    esp_err_t audio_service_write_pcm_stream_borrow(uint64_t stream_id, const int16_t *samples, size_t sample_count,
                                                    uint32_t timeout_ms, size_t *out_written);

    /**
 * @brief 同步排空或丢弃并关闭 PCM 流
 * @param[in] stream_id 活动流 ID
 * @param[in] discard true 立即丢弃，false 排空缓冲
 * @param[in] timeout_ms 等待输出关闭的最长时间
 * @return ESP_OK 已关闭；ESP_ERR_TIMEOUT 清理仍在推进，可用同一 ID 重试；其他值表示状态错误
 */
    esp_err_t audio_service_close_pcm_stream(uint64_t stream_id, bool discard, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
