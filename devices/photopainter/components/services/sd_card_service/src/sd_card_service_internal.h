/**
 * @file sd_card_service_internal.h
 * @brief SD 卡监测 Service 内部契约
 */
#pragma once

#include "esp_err.h"

/** @brief 创建插拔监测 Task 并注册 Device ISR 回调 */
esp_err_t sd_card_service_task_start(void);

/** @brief 注销 Device ISR 回调并同步停止插拔监测 Task */
esp_err_t sd_card_service_task_stop(void);

/** @brief 根据当前物理插卡状态同步挂载或卸载 */
esp_err_t sd_card_service_reconcile_card(void);
