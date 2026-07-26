/*
 * 文件职责：把 Firmware OTA 事实转换为设置页可读取的线程安全快照。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "firmware_ota.h"

/** @brief 设置页 OTA 子页面的稳定呈现状态 */
typedef enum
{
    OTA_PRESENTER_STATE_IDLE = 0,       /*!< 尚未发起或会话已清理 */
    OTA_PRESENTER_STATE_CHECKING,       /*!< 正在检查更新 */
    OTA_PRESENTER_STATE_NO_UPDATE,      /*!< 当前版本已是最新 */
    OTA_PRESENTER_STATE_AVAILABLE,      /*!< 已缓存目标，等待用户确认安装 */
    OTA_PRESENTER_STATE_DOWNLOADING,    /*!< 正在下载并安装 */
    OTA_PRESENTER_STATE_CHECK_FAILED,   /*!< 检查事务失败 */
    OTA_PRESENTER_STATE_INSTALL_FAILED, /*!< 异步安装事务失败，目标已不可重试 */
} ota_presenter_state_t;

/** @brief OTA 子页面按值读取的不可变 View Model */
typedef struct
{
    ota_presenter_state_t state;                                     /*!< 当前呈现状态 */
    char                  current_version[FIRMWARE_OTA_VERSION_MAX]; /*!< 当前固件版本 */
    char                  target_version[FIRMWARE_OTA_VERSION_MAX];  /*!< 待安装目标版本 */
    size_t                target_size_bytes;                         /*!< 目标固件字节数 */
    esp_err_t             error;                                     /*!< 最近一次失败码 */
} ota_presenter_view_model_t;

/**
 * @brief 初始化 OTA Presenter
 *
 * 初始化为空闲快照并采集当前固件版本。
 *
 * @return ESP_OK 初始化成功
 */
esp_err_t ota_presenter_init(void);

/**
 * @brief 将快照切换为正在检查 OTA 更新
 */
void ota_presenter_show_checking(void);

/**
 * @brief 展示正在下载 OTA 固件
 */
void ota_presenter_show_downloading(void);

/**
 * @brief 记录检查命令提交前失败
 *
 * @param[in] error 检查请求失败码
 * @param[in] manual true 为手动检查，需要向用户显示失败；false 为自动检查并回到空闲
 */
void ota_presenter_show_check_request_failed(esp_err_t error, bool manual);

/**
 * @brief 保留目标信息并记录安装命令提交失败
 *
 * @param[in] error 安装提交失败码
 */
void ota_presenter_show_install_submit_failed(esp_err_t error);

/**
 * @brief 保留目标信息并记录丢弃目标失败
 *
 * @param[in] error 目标清理失败码
 */
void ota_presenter_show_discard_failed(esp_err_t error);

/**
 * @brief 清空当前 OTA 会话并恢复空闲快照
 */
void ota_presenter_reset(void);

/**
 * @brief 按值读取线程安全 OTA View Model
 *
 * @param[out] out_view 快照输出地址
 * @return ESP_OK 已复制；ESP_ERR_INVALID_ARG 输出地址为空
 */
esp_err_t ota_presenter_get_view_copy(ota_presenter_view_model_t *out_view);

/**
 * @brief 按值解释 Firmware OTA 完成事件并更新快照
 *
 * @param[in] event Firmware OTA 完成事件
 * @param[in] manual true 表示手动检查结果；false 表示自动检查结果
 */
void ota_presenter_handle_firmware_event_copy(const firmware_ota_event_t *event, bool manual);
