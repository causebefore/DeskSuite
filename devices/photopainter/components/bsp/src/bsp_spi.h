/**
 * @file bsp_spi.h
 * @brief BSP 内部共享 SPI2 总线生命周期契约
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 为一个板级设备取得共享 SPI2 总线引用
 *
 * 第一个调用方初始化总线，后续调用方只增加引用。装配方必须串行调用生命周期 API。
 *
 * @return ESP_OK 已取得引用；ESP_ERR_INVALID_STATE 引用计数溢出；或 SPI 驱动错误码
 */
esp_err_t bsp_spi_acquire(void);

/**
 * @brief 释放一个共享 SPI2 总线引用
 *
 * 最后一个调用方释放时才真正关闭 SPI 驱动总线。
 *
 * @return ESP_OK 已释放；ESP_ERR_INVALID_STATE 当前没有引用；或 SPI 驱动错误码
 */
esp_err_t bsp_spi_release(void);
