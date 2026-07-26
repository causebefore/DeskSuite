/**
 * @file device_audio.c
 * @brief 把板级音频资源收敛为稳定的 Device API
 */
#include "device_audio.h"

#include "bsp.h"

static bool s_initialized;

esp_err_t device_audio_init(const device_audio_config_t *config)
{
    if (s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL || config->sample_rate_hz == 0U || config->initial_volume > 100U || config->input_gain_db > 48U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const bsp_audio_config_t bsp_config = {
        .sample_rate_hz = config->sample_rate_hz,
        .initial_volume = config->initial_volume,
        .input_gain_db  = config->input_gain_db,
    };
    const esp_err_t error = bsp_audio_init(&bsp_config);
    if (error == ESP_OK)
    {
        s_initialized = true;
    }
    return error;
}

esp_err_t device_audio_deinit(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = bsp_audio_deinit();
    if (error == ESP_OK)
    {
        s_initialized = false;
    }
    return error;
}

esp_err_t device_audio_enable_output(bool enable)
{
    return s_initialized ? bsp_audio_enable_output(enable) : ESP_ERR_INVALID_STATE;
}

esp_err_t device_audio_enable_input(bool enable)
{
    return s_initialized ? bsp_audio_enable_input(enable) : ESP_ERR_INVALID_STATE;
}

esp_err_t device_audio_set_output_volume(int volume)
{
    return s_initialized ? bsp_audio_set_output_volume(volume) : ESP_ERR_INVALID_STATE;
}

esp_err_t device_audio_write(const int16_t *data, size_t sample_count, size_t *out_written)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_audio_write(data, sample_count, out_written);
}

esp_err_t device_audio_read(int16_t *dest, size_t sample_count, size_t *out_read)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_audio_read(dest, sample_count, out_read);
}

uint32_t device_audio_get_sample_rate_hz(void)
{
    return s_initialized ? bsp_audio_get_sample_rate_hz() : 0U;
}
