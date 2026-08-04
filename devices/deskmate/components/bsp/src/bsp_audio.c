#include "bsp.h"

#include <limits.h>
#include <string.h>

#include "board.h"
#include "bsp_i2c_internal.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_audio";

/* DMA 描述符/帧数，沿用小智 AudioCodec 默认值 */
#define BSP_AUDIO_DMA_DESC_NUM                  6
#define BSP_AUDIO_DMA_FRAME_NUM                 240
#define BSP_AUDIO_WRITE_TIMEOUT_MS              1000U
#define BSP_AUDIO_ES8311_DAC_MUTE_REG           0x31
#define BSP_AUDIO_ES8311_DAC_VOLUME_REG         0x32
#define BSP_AUDIO_ES8311_DAC_MUTE_MASK          0x60
#define BSP_AUDIO_OUTPUT_SELF_TEST_VOLUME       100
#define BSP_AUDIO_OUTPUT_SELF_TEST_FREQUENCY_HZ 1000U
#define BSP_AUDIO_OUTPUT_SELF_TEST_DURATION_MS  300U
#define BSP_AUDIO_OUTPUT_SELF_TEST_SETTLE_MS    30U
#define BSP_AUDIO_OUTPUT_SELF_TEST_DRAIN_MS     80U
#define BSP_AUDIO_OUTPUT_SELF_TEST_AMPLITUDE    30000
/* 默认音量与增益见 Kconfig: DeskMate Audio/Voice */
/* ES7210 双麦通道掩码（mic1+mic2）。channel_mask 决定 I2S TDM 的 active_slot =
 * popcount(mask)（见 i2s_tdm.c），即 DMA 每帧实际交付的通道数，必须与 ES7210 的
 * mic_selected、上层 APS_HW_CHANNELS 一致。
 * 双麦用 2 位(0x3)：duplex 下 TX/RX 共享 WS，slot 数必须相同，input 2 slot 才能让
 * TX 回到标准 16-bit stereo（slot_bit=16）；若 input 4 slot 会逼 TX slot_bit=32/64
 * 错位、播放爆音。AEC 参考通道取舍由 AFE 的 aec_init 控制，与此处无关。 */
#define BSP_AUDIO_INPUT_CH_MASK (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))

/* I2S 双工通道句柄：TX 喂 ES8311，RX 收 ES7210，共用 MCLK/BCLK/WS */
static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;

/* esp_codec_dev 各层接口：data/ctrl/gpio/codec/device */
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_out_ctrl_if;
static const audio_codec_if_t      *s_out_codec_if;
static const audio_codec_ctrl_if_t *s_in_ctrl_if;
static const audio_codec_if_t      *s_in_codec_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static esp_codec_dev_handle_t       s_output_dev;
static esp_codec_dev_handle_t       s_input_dev;

static bool               s_ready;
static bool               s_input_enabled;
static bool               s_output_enabled;
static int                s_output_volume;
static bsp_audio_config_t s_config;

/**
 * @brief 显式设置并回读功放使能脚，避免 Codec 驱动忽略 GPIO 配置或写入错误
 *
 * GPIO 输入路径必须同时打开，否则 ESP32-S3 的纯输出模式无法通过 gpio_get_level()
 * 验证实际电平。该函数仅在输出生命周期切换时调用，不参与 PCM 热路径。
 *
 * @param[in] enabled true 拉高功放使能，false 拉低
 * @return ESP_OK 电平已提交且回读一致；其他值表示 GPIO 操作失败
 */
static esp_err_t set_output_pa(bool enabled)
{
    const gpio_num_t    pa_gpio  = (gpio_num_t) BOARD_AUDIO_PA_PIN;
    const int           expected = enabled ? 1 : 0;
    const gpio_config_t config   = {
        .pin_bit_mask = 1ULL << BOARD_AUDIO_PA_PIN,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "配置扬声器功放 GPIO 失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(pa_gpio, expected), TAG, "设置扬声器功放电平失败");
    const int actual = gpio_get_level(pa_gpio);
    ESP_RETURN_ON_FALSE(actual == expected,
                        ESP_FAIL,
                        TAG,
                        "扬声器功放 GPIO%d 电平校验失败: expected=%d actual=%d",
                        BOARD_AUDIO_PA_PIN,
                        expected,
                        actual);
    ESP_LOGI(TAG, "扬声器功放状态: GPIO%d=%d", BOARD_AUDIO_PA_PIN, actual);
    return ESP_OK;
}

/**
 * @brief 回读 ES8311 的静音和音量寄存器，验证上层配置确实写入硬件
 *
 * esp_codec_dev 的音量/静音包装接口不会传播底层 Codec 写寄存器错误，因此必须回读
 * 关键寄存器，避免输出链路在寄存器写失败后仍报告成功。
 *
 * @return ESP_OK DAC 已取消静音且非零请求音量对应非零寄存器；否则返回 ESP_FAIL
 */
static esp_err_t verify_output_codec_state(void)
{
    int mute_reg    = 0;
    int volume_reg  = 0;
    int codec_error = esp_codec_dev_read_reg(s_output_dev, BSP_AUDIO_ES8311_DAC_MUTE_REG, &mute_reg);
    codec_error |= esp_codec_dev_read_reg(s_output_dev, BSP_AUDIO_ES8311_DAC_VOLUME_REG, &volume_reg);
    if (codec_error != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(TAG, "读取 ES8311 输出状态失败: ret=%d", codec_error);
        return ESP_FAIL;
    }

    if ((mute_reg & BSP_AUDIO_ES8311_DAC_MUTE_MASK) != 0)
    {
        ESP_LOGE(TAG, "ES8311 DAC 仍处于静音状态: reg31=0x%02x", mute_reg);
        return ESP_FAIL;
    }
    if (s_output_volume > 0 && volume_reg == 0)
    {
        ESP_LOGE(TAG, "ES8311 DAC 音量未写入: requested=%d reg32=0x%02x", s_output_volume, volume_reg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ES8311 输出状态: volume=%d reg31=0x%02x reg32=0x%02x", s_output_volume, mute_reg, volume_reg);
    return ESP_OK;
}

/* 创建 I2S0 双工通道：TX 标准 I2S（ES8311 播放），RX TDM（ES7210 双麦录音） */
static esp_err_t create_duplex_channels(uint32_t sample_rate_hz)
{
    i2s_chan_config_t chan_cfg = {
        .id                   = I2S_NUM_0,
        .role                 = I2S_ROLE_MASTER,
        .dma_desc_num         = BSP_AUDIO_DMA_DESC_NUM,
        .dma_frame_num        = BSP_AUDIO_DMA_FRAME_NUM,
        .auto_clear_after_cb  = true,
        .auto_clear_before_cb = false,
        .intr_priority        = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG, "创建 I2S 通道失败");

    /* TX：标准 I2S 模式，喂 ES8311（DAC 播放，立体声） */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate_hz,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = (gpio_num_t)BOARD_AUDIO_I2S_PIN_MCLK,
            .bclk = (gpio_num_t)BOARD_AUDIO_I2S_PIN_BCLK,
            .ws = (gpio_num_t)BOARD_AUDIO_I2S_PIN_WS,
            .dout = (gpio_num_t)BOARD_AUDIO_I2S_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "初始化 I2S TX(标准模式)失败");

    /* RX：TDM 模式，收 ES7210（双麦，2-slot） */
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate_hz,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                               I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = (gpio_num_t)BOARD_AUDIO_I2S_PIN_MCLK,
            .bclk = (gpio_num_t)BOARD_AUDIO_I2S_PIN_BCLK,
            .ws = (gpio_num_t)BOARD_AUDIO_I2S_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)BOARD_AUDIO_I2S_PIN_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx_handle, &tdm_cfg), TAG, "初始化 I2S RX(TDM模式)失败");

    ESP_LOGI(TAG, "I2S0 双工通道创建完成 (TX=标准模式->ES8311, RX=TDM->ES7210)");
    return ESP_OK;
}

/**
 * @brief 逆序释放全部已创建的音频资源，也可用于部分初始化失败回滚
 */
static void release_audio_resources(void)
{
    if (s_input_enabled && s_input_dev != NULL)
    {
        (void) esp_codec_dev_close(s_input_dev);
    }
    if (s_output_enabled && s_output_dev != NULL)
    {
        (void) esp_codec_dev_close(s_output_dev);
        (void) set_output_pa(false);
    }
    s_input_enabled  = false;
    s_output_enabled = false;

    if (s_input_dev != NULL)
    {
        esp_codec_dev_delete(s_input_dev);
        s_input_dev = NULL;
    }
    if (s_output_dev != NULL)
    {
        esp_codec_dev_delete(s_output_dev);
        s_output_dev = NULL;
    }
    if (s_in_codec_if != NULL)
    {
        (void) audio_codec_delete_codec_if(s_in_codec_if);
        s_in_codec_if = NULL;
    }
    if (s_in_ctrl_if != NULL)
    {
        (void) audio_codec_delete_ctrl_if(s_in_ctrl_if);
        s_in_ctrl_if = NULL;
    }
    if (s_out_codec_if != NULL)
    {
        (void) audio_codec_delete_codec_if(s_out_codec_if);
        s_out_codec_if = NULL;
    }
    if (s_out_ctrl_if != NULL)
    {
        (void) audio_codec_delete_ctrl_if(s_out_ctrl_if);
        s_out_ctrl_if = NULL;
    }
    if (s_gpio_if != NULL)
    {
        (void) audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_data_if != NULL)
    {
        (void) audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_rx_handle != NULL)
    {
        (void) i2s_channel_disable(s_rx_handle);
        (void) i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
    if (s_tx_handle != NULL)
    {
        (void) i2s_channel_disable(s_tx_handle);
        (void) i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }

    memset(&s_config, 0, sizeof(s_config));
    s_output_volume = 0;
    s_ready         = false;
}

/**
 * @brief 以最大硬件音量向 I2S TX 直接写入短音调，隔离上层播放链路进行开机诊断
 *
 * 自检只复用 BSP 的 Codec 启停和同步 I2S 写入，不经过 Audio Service、解码器、
 * 采样率转换或流缓冲。结束时关闭输出并恢复调用前音量。
 *
 * @return ESP_OK 音调完整提交并恢复输出状态；其他值表示板级输出链路操作失败
 */
static esp_err_t run_output_self_test(void)
{
    const int original_volume = s_output_volume;
    esp_err_t result          = ESP_OK;

    ESP_LOGW(TAG,
             "开始扬声器直驱自检: volume=%d frequency=%uHz duration=%ums",
             BSP_AUDIO_OUTPUT_SELF_TEST_VOLUME,
             BSP_AUDIO_OUTPUT_SELF_TEST_FREQUENCY_HZ,
             BSP_AUDIO_OUTPUT_SELF_TEST_DURATION_MS);

    if (s_config.sample_rate_hz < 2U * BSP_AUDIO_OUTPUT_SELF_TEST_FREQUENCY_HZ)
    {
        ESP_LOGE(TAG, "扬声器直驱自检采样率过低: rate=%u", (unsigned) s_config.sample_rate_hz);
        return ESP_ERR_INVALID_ARG;
    }

    result = bsp_audio_set_output_volume(BSP_AUDIO_OUTPUT_SELF_TEST_VOLUME);
    if (result == ESP_OK)
    {
        result = bsp_audio_enable_output(true);
    }
    if (result == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(BSP_AUDIO_OUTPUT_SELF_TEST_SETTLE_MS));

        int16_t      tone[BSP_AUDIO_DMA_FRAME_NUM];
        uint32_t     phase_hz = 0U;
        const size_t total_samples =
            (size_t) (((uint64_t) s_config.sample_rate_hz * BSP_AUDIO_OUTPUT_SELF_TEST_DURATION_MS) / 1000U);
        size_t submitted = 0U;

        while (submitted < total_samples)
        {
            const size_t remaining     = total_samples - submitted;
            const size_t block_samples = remaining < BSP_AUDIO_DMA_FRAME_NUM ? remaining : BSP_AUDIO_DMA_FRAME_NUM;
            for (size_t index = 0U; index < block_samples; ++index)
            {
                tone[index] = phase_hz < (s_config.sample_rate_hz / 2U)
                                  ? (int16_t) BSP_AUDIO_OUTPUT_SELF_TEST_AMPLITUDE
                                  : (int16_t) -BSP_AUDIO_OUTPUT_SELF_TEST_AMPLITUDE;
                phase_hz += BSP_AUDIO_OUTPUT_SELF_TEST_FREQUENCY_HZ;
                if (phase_hz >= s_config.sample_rate_hz)
                {
                    phase_hz -= s_config.sample_rate_hz;
                }
            }

            size_t written = 0U;
            result         = bsp_audio_write(tone, block_samples, &written);
            if (result != ESP_OK || written != block_samples)
            {
                ESP_LOGE(TAG,
                         "扬声器直驱自检写入失败: submitted=%u requested=%u written=%u error=%s",
                         (unsigned) submitted,
                         (unsigned) block_samples,
                         (unsigned) written,
                         esp_err_to_name(result));
                if (result == ESP_OK)
                {
                    result = ESP_FAIL;
                }
                break;
            }
            submitted += written;
        }

        if (result == ESP_OK)
        {
            memset(tone, 0, sizeof(tone));
            size_t silence_written = 0U;
            result                 = bsp_audio_write(tone, BSP_AUDIO_DMA_FRAME_NUM, &silence_written);
            if (result == ESP_OK && silence_written != BSP_AUDIO_DMA_FRAME_NUM)
            {
                result = ESP_FAIL;
            }
            vTaskDelay(pdMS_TO_TICKS(BSP_AUDIO_OUTPUT_SELF_TEST_DRAIN_MS));
        }
    }

    if (s_output_enabled)
    {
        const esp_err_t close_result = bsp_audio_enable_output(false);
        if (result == ESP_OK)
        {
            result = close_result;
        }
    }
    const esp_err_t restore_result = bsp_audio_set_output_volume(original_volume);
    if (result == ESP_OK)
    {
        result = restore_result;
    }

    if (result == ESP_OK)
    {
        ESP_LOGW(TAG, "扬声器直驱自检完成，已恢复默认音量=%d", original_volume);
    }
    else
    {
        ESP_LOGE(TAG, "扬声器直驱自检失败: %s", esp_err_to_name(result));
    }
    return result;
}

esp_err_t bsp_audio_init(const bsp_audio_config_t *config)
{
    if (s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(config != NULL && config->sample_rate_hz > 0U && config->initial_volume <= 100U
                            && config->input_gain_db <= 48U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "音频初始化参数非法");

    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C 总线未就绪: %s", esp_err_to_name(result));
        return result;
    }

    /* 音频 codec 与其它外设共用 bsp_i2c 的总线，先确保总线就绪 */
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_bus_handle();
    if (i2c_bus == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    result = create_duplex_channels(config->sample_rate_hz);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "创建 I2S 双工通道失败: %s", esp_err_to_name(result));
        goto fail;
    }

    /* data_if：绑定 I2S0 的 tx/rx 句柄 */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* gpio_if：用于控制 PA 使能脚 */
    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* ---- 输出：ES8311（DAC，扬声器播放） ---- */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = (i2c_port_t) BOARD_AUDIO_I2C_PORT,
        /* esp_codec_dev 1.5.x 的 I2C Master 适配层接收 8-bit 地址并在内部右移。 */
        .addr       = (uint8_t) (BOARD_AUDIO_ES8311_ADDR_7BIT << 1U),
        .bus_handle = i2c_bus,
    };
    s_out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_out_ctrl_if == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    es8311_codec_cfg_t es8311_cfg        = {};
    es8311_cfg.ctrl_if                   = s_out_ctrl_if;
    es8311_cfg.gpio_if                   = s_gpio_if;
    es8311_cfg.codec_mode                = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin                    = BOARD_AUDIO_PA_PIN;
    es8311_cfg.pa_reverted               = false;
    es8311_cfg.use_mclk                  = true;
    es8311_cfg.hw_gain.pa_voltage        = 5.0f;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3f;
    s_out_codec_if                       = es8311_codec_new(&es8311_cfg);
    if (s_out_codec_if == NULL)
    {
        result = ESP_FAIL;
        goto fail;
    }

    esp_codec_dev_cfg_t out_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_out_codec_if,
        .data_if  = s_data_if,
    };
    s_output_dev = esp_codec_dev_new(&out_dev_cfg);
    if (s_output_dev == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* ---- 输入：ES7210（ADC，双通道麦克风 mic1+mic2） ---- */
    i2c_cfg.addr = (uint8_t) (BOARD_AUDIO_ES7210_ADDR_7BIT << 1U);
    s_in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_in_ctrl_if == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if            = s_in_ctrl_if;
    /* 双麦：仅选 MIC1+MIC2，与 I2S TDM 2-slot 对齐（duplex 下与 TX 共享 WS） */
    es7210_cfg.mic_selected       = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
    s_in_codec_if                 = es7210_codec_new(&es7210_cfg);
    if (s_in_codec_if == NULL)
    {
        result = ESP_FAIL;
        goto fail;
    }

    esp_codec_dev_cfg_t in_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_in_codec_if,
        .data_if  = s_data_if,
    };
    s_input_dev = esp_codec_dev_new(&in_dev_cfg);
    if (s_input_dev == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_config        = *config;
    s_output_volume = config->initial_volume;
    s_ready         = true;
    result          = run_output_self_test();
    if (result != ESP_OK)
    {
        goto fail;
    }
    ESP_LOGI(TAG, "音频初始化完成: ES8311(播放) + ES7210(录音), 采样率=%dHz", (int) s_config.sample_rate_hz);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "音频初始化失败，回滚已创建资源: %s", esp_err_to_name(result));
    release_audio_resources();
    return result;
}

esp_err_t bsp_audio_enable_output(bool enable)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == s_output_enabled)
    {
        return ESP_OK;
    }
    if (enable)
    {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel         = 1,
            .channel_mask    = 0,
            .sample_rate     = s_config.sample_rate_hz,
        };
        esp_err_t result = esp_codec_dev_open(s_output_dev, &fs);
        if (result != ESP_OK)
        {
            (void) esp_codec_dev_close(s_output_dev);
            ESP_LOGE(TAG, "打开输出设备失败: %s", esp_err_to_name(result));
            return result;
        }
        ESP_LOGI(TAG,
                 "输出 open: channel=%u mask=0x%x rate=%u bits=%u",
                 fs.channel,
                 fs.channel_mask,
                 fs.sample_rate,
                 fs.bits_per_sample);

        result = set_output_pa(true);
        if (result == ESP_OK)
        {
            result = esp_codec_dev_set_out_mute(s_output_dev, false);
        }
        if (result == ESP_OK)
        {
            result = esp_codec_dev_set_out_vol(s_output_dev, s_output_volume);
        }
        if (result == ESP_OK)
        {
            result = verify_output_codec_state();
        }
        if (result != ESP_OK)
        {
            (void) set_output_pa(false);
            (void) esp_codec_dev_close(s_output_dev);
            ESP_LOGE(TAG, "使能扬声器输出失败: %s", esp_err_to_name(result));
            return result;
        }
    }
    else
    {
        const esp_err_t close_error = esp_codec_dev_close(s_output_dev);
        const esp_err_t pa_error    = set_output_pa(false);
        if (close_error != ESP_OK || pa_error != ESP_OK)
        {
            const esp_err_t result = close_error != ESP_OK ? close_error : pa_error;
            ESP_LOGE(TAG, "关闭扬声器输出失败: %s", esp_err_to_name(result));
            return result;
        }
    }
    s_output_enabled = enable;
    ESP_LOGI(TAG, "输出%s", enable ? "已使能" : "已关闭");
    return ESP_OK;
}

esp_err_t bsp_audio_enable_input(bool enable)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == s_input_enabled)
    {
        return ESP_OK;
    }
    if (enable)
    {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel         = 2,
            .channel_mask    = BSP_AUDIO_INPUT_CH_MASK,
            .sample_rate     = s_config.sample_rate_hz,
        };
        ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_input_dev, &fs), TAG, "打开输入设备失败");
        ESP_LOGI(TAG,
                 "输入 open: channel=%u mask=0x%x rate=%u bits=%u",
                 fs.channel,
                 fs.channel_mask,
                 fs.sample_rate,
                 fs.bits_per_sample);
        /* esp_codec_dev_open 内部 enable + _update_codec_setting 会用 dev->mic_gain(=0)
         * 把 ES7210 双麦重置为 0dB，必须在此之后重设；双麦同增益，保证 AFE
         * (取 ch0+ch1) 幅度匹配。 */
        uint16_t        gain_mask  = BSP_AUDIO_INPUT_CH_MASK;
        const esp_err_t gain_error = esp_codec_dev_set_in_channel_gain(s_input_dev, gain_mask, s_config.input_gain_db);
        if (gain_error != ESP_OK)
        {
            (void) esp_codec_dev_close(s_input_dev);
            return gain_error;
        }
    }
    else
    {
        ESP_RETURN_ON_ERROR(esp_codec_dev_close(s_input_dev), TAG, "关闭输入设备失败");
    }
    s_input_enabled = enable;
    ESP_LOGI(TAG, "输入%s", enable ? "已使能" : "已关闭");
    return ESP_OK;
}

esp_err_t bsp_audio_set_output_volume(int volume)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(volume >= 0 && volume <= 100, ESP_ERR_INVALID_ARG, TAG, "输出音量超出 0～100");
    s_output_volume = volume;
    if (s_output_enabled)
    {
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_output_dev, volume), TAG, "设置输出音量失败");
    }
    return ESP_OK;
}

esp_err_t bsp_audio_write(const int16_t *data, size_t sample_count, size_t *out_written)
{
    ESP_RETURN_ON_FALSE(data != NULL && out_written != NULL && sample_count > 0U
                            && sample_count <= (size_t) INT_MAX / sizeof(int16_t),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "播放写入参数非法");
    ESP_RETURN_ON_FALSE(s_ready && s_output_enabled, ESP_ERR_INVALID_STATE, TAG, "音频输出未启用");
    *out_written                 = 0U;
    const size_t requested_bytes = sample_count * sizeof(int16_t);
    size_t       written_bytes   = 0U;

    /* Codec 的启停、格式和硬件音量仍由 esp_codec_dev 管理；PCM 数据直接提交给 BSP
     * 拥有的 TX 句柄，才能校验驱动实际接收的字节数，避免上层包装成功但未写入数据。 */
    const esp_err_t err =
        i2s_channel_write(s_tx_handle, data, requested_bytes, &written_bytes, BSP_AUDIO_WRITE_TIMEOUT_MS);
    *out_written = written_bytes / sizeof(int16_t);
    if (err != ESP_OK || written_bytes != requested_bytes)
    {
        ESP_LOGW(TAG,
                 "I2S 播放写入不完整: requested=%u actual=%u err=%s",
                 (unsigned) requested_bytes,
                 (unsigned) written_bytes,
                 esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_read(int16_t *dest, size_t sample_count, size_t *out_read)
{
    ESP_RETURN_ON_FALSE(dest != NULL && out_read != NULL && sample_count > 0U
                            && sample_count <= (size_t) INT_MAX / sizeof(int16_t),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "录音读取参数非法");
    ESP_RETURN_ON_FALSE(s_ready && s_input_enabled, ESP_ERR_INVALID_STATE, TAG, "音频输入未启用");
    *out_read           = 0U;
    const esp_err_t err = esp_codec_dev_read(s_input_dev, (void *) dest, sample_count * sizeof(int16_t));
    if (err != ESP_OK)
    {
        return err;
    }
    *out_read = sample_count;
    return ESP_OK;
}

esp_err_t bsp_audio_deinit(void)
{
    if (!s_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }

    release_audio_resources();
    ESP_LOGI(TAG, "音频反初始化完成: ES8311 + ES7210 + I2S0 已释放");
    return ESP_OK;
}

uint32_t bsp_audio_get_sample_rate_hz(void)
{
    return s_ready ? s_config.sample_rate_hz : 0U;
}
