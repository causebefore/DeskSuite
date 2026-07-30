/**
 * @file bsp_i2c_internal.h
 * @brief BSP 内部共享 I2C 总线接口
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t               bsp_i2c_init(void);
esp_err_t               bsp_i2c_deinit(void);
i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void);
