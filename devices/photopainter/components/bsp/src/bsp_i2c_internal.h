/**
 * @file bsp_i2c_internal.h
 * @brief BSP 内部共享 I2C 总线所有权接口
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/**
 * @brief 取得共享 I2C 总线并增加使用计数
 *
 * 第一个使用者会创建总线并立即执行 ESP-IDF master bus reset。
 *
 * @param[out] out_bus 共享总线句柄，仅在 ESP_OK 时有效
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；或底层错误码
 */
esp_err_t bsp_i2c_acquire(i2c_master_bus_handle_t *out_bus);

/**
 * @brief 释放一次共享 I2C 总线使用权
 *
 * 最后一个使用者释放时删除总线；删除失败时保留所有权和使用计数。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 没有使用者；或底层错误码
 */
esp_err_t bsp_i2c_release(void);
