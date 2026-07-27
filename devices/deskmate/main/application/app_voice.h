/*
 * 文件职责：声明语音会话 Application 的产品意图接口。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_button.h"
#include "esp_err.h"

/** @brief 语音 Application 可逆运行状态。 */
typedef enum
{
    APP_VOICE_STATE_UNINITIALIZED = 0, /*!< 尚未初始化 */
    APP_VOICE_STATE_STOPPED,           /*!< 依赖资源保留，拒绝按键语音 */
    APP_VOICE_STATE_STARTING,          /*!< 正在按依赖顺序启动 */
    APP_VOICE_STATE_RUNNING,           /*!< 允许语音页按键发起会话 */
    APP_VOICE_STATE_STOPPING,          /*!< 正在反序停止语音链 */
    APP_VOICE_STATE_FAILED,            /*!< 生命周期或回滚失败 */
} app_voice_state_t;

/** @brief 语音 Application 聚合状态快照。 */
typedef struct
{
    app_voice_state_t state;               /*!< Application 生命周期状态 */
    bool              session_busy;        /*!< 是否存在活动语音回合 */
    bool              processor_idle;      /*!< AFE 无采集且 Task 已停泊 */
    bool              input_active;        /*!< 麦克风输入是否开启 */
    bool              output_active;       /*!< 扬声器输出是否开启 */
    bool              network_lease_held;  /*!< 是否仍持有实时语音网络租约 */
    esp_err_t         primary_error;       /*!< 最近主操作错误 */
    esp_err_t         recovery_error;      /*!< 最近回滚错误 */
} app_voice_status_t;

/**
 * @brief 初始化语音会话 Application
 *
 * 注册语音终态事件，创建生命周期同步资源并初始化网络租约状态。
 *
 * @return ESP_OK 成功；其他值表示初始化失败
 */
esp_err_t app_voice_init(void);

/**
 * @brief 按 Audio → AFE → Voice 顺序可逆启动语音 Runtime
 *
 * 返回 ESP_OK 时仅开放按键会话入口，麦克风和扬声器仍保持关闭。
 *
 * @param[in] timeout_ms 整体启动与失败回滚超时
 * @return ESP_OK 已进入 RUNNING；ESP_ERR_INVALID_ARG 超时为零；
 *         ESP_ERR_INVALID_STATE 生命周期不允许；其他值表示启动或回滚失败
 */
esp_err_t app_voice_start(uint32_t timeout_ms);

/**
 * @brief 在会话和网络租约空闲时反序停止语音 Runtime
 *
 * 本函数不取消活动语音会话。返回 ESP_OK 时 AFE Task 已停泊，输入输出均已关闭。
 *
 * @param[in] timeout_ms 整体停止超时
 * @return ESP_OK 已进入 STOPPED；ESP_ERR_INVALID_ARG 超时为零；
 *         ESP_ERR_INVALID_STATE 会话、租约或生命周期不允许；其他值表示停止或回滚失败
 */
esp_err_t app_voice_stop(uint32_t timeout_ms);

/**
 * @brief 从 STOPPED 注销事件并释放语音 Application 生命周期资源
 *
 * @return ESP_OK 已反初始化；ESP_ERR_INVALID_STATE 生命周期不允许
 */
esp_err_t app_voice_deinit(void);

/**
 * @brief 复制语音 Runtime 及下层活动状态
 *
 * @param[out] out_status 状态输出
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 输出为空；ESP_ERR_INVALID_STATE 尚未初始化
 */
esp_err_t app_voice_get_status_copy(app_voice_status_t *out_status);

/**
 * @brief 处理语音页按键事件
 *
 * 在语音页激活时，将按键事件转换为语音业务语义（如长按触发录音）。
 *
 * @param[in] key_event 按键事件
 * @return true 已处理；false 未处理（需上报给 App 输入入口）
 */
bool app_voice_consume_input(device_button_event_t key_event);
