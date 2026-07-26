/**
 * @file bsp_i2c_internal.h
 * @brief BSP 内部共享 I2C 总线接口
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t bsp_i2c_init(void);
esp_err_t bsp_i2c_deinit(void);
esp_err_t bsp_i2c_write(uint8_t address, const uint8_t *data, size_t size_bytes);
esp_err_t bsp_i2c_read(uint8_t address, uint8_t *data, size_t size_bytes);
esp_err_t bsp_i2c_write_read(uint8_t address, const uint8_t *write_data, size_t write_size_bytes, uint8_t *read_data,
                             size_t read_size_bytes);
i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void);
