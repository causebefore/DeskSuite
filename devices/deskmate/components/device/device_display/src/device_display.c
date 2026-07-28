/**
 * @file device_display.c
 * @brief 把板级 RLCD 资源收敛为稳定的 Device API
 */
#include "device_display.h"

#include <stdbool.h>

#include "bsp.h"

static bool s_initialized;
static bool s_running;

esp_err_t device_display_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }
    const esp_err_t error = bsp_display_init();
    if (error == ESP_OK)
    {
        s_initialized = true;
        s_running     = true;
    }
    return error;
}

esp_err_t device_display_stop(uint32_t timeout_ms)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_running)
    {
        return ESP_OK;
    }
    const esp_err_t error = bsp_display_stop(timeout_ms);
    if (error == ESP_OK)
    {
        s_running = false;
    }
    return error;
}

esp_err_t device_display_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running)
    {
        return ESP_OK;
    }
    const esp_err_t error = bsp_display_start();
    if (error == ESP_OK)
    {
        s_running = true;
    }
    return error;
}

esp_err_t device_display_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = bsp_display_deinit();
    if (error == ESP_OK)
    {
        s_initialized = false;
        s_running     = false;
    }
    return error;
}

esp_err_t device_display_get_info_copy(device_display_info_t *out_info)
{
    if (out_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    bsp_display_info_t bsp_info;
    const esp_err_t    error = bsp_display_get_info_copy(&bsp_info);
    if (error == ESP_OK)
    {
        out_info->width_pixels  = bsp_info.width_pixels;
        out_info->height_pixels = bsp_info.height_pixels;
    }
    return error;
}

esp_err_t device_display_write_i1_area(int x1, int y1, int x2, int y2, const uint8_t *pixels, uint32_t stride_bytes)
{
    return s_initialized && s_running ? bsp_display_write_i1_area(x1, y1, x2, y2, pixels, stride_bytes)
                                      : ESP_ERR_INVALID_STATE;
}

esp_err_t device_display_request_flush(void)
{
    return s_initialized && s_running ? bsp_display_request_flush() : ESP_ERR_INVALID_STATE;
}

esp_err_t device_display_wait_flush_done(uint32_t timeout_ms)
{
    return s_initialized ? bsp_display_wait_flush_done(timeout_ms) : ESP_ERR_INVALID_STATE;
}

uint32_t device_display_get_flush_fps(void)
{
    return s_initialized ? bsp_display_get_flush_fps() : 0;
}

uint32_t device_display_get_total_flush_count(void)
{
    return s_initialized ? bsp_display_get_total_flush_count() : 0;
}
