#include "audio_processor_service.h"
#include "audio_processor_service_internal.h"

#include <stdlib.h>
#include <string.h>

#include "audio_service.h"
#include "esp_ae_rate_cvt.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio_processor";

ESP_EVENT_DEFINE_BASE(AUDIO_PROCESSOR_EVENT);

/* 硬件采样率（ES7210 = 24kHz），AFE 需要 16kHz。
 * 双麦 2 通道：duplex 下 TX/RX 共享 WS，slot 数必须一致，input 2 slot 才能让
 * TX 保持标准 16-bit stereo，避免 slot_bit 被放大到 32/64 导致播放爆音。 */
#define APS_HW_CHANNELS          2
#define APS_HW_SAMPLE_RATE       24000
#define APS_AFE_SAMPLE_RATE      16000
/* AFE 输入：双麦（"MM" 格式） */
#define APS_AFE_MIC_CHANNELS     2

#define APS_FETCH_WAIT_MS        200
/* AFE drain 超时见 Kconfig: DeskMate Audio/Voice */

/* feed_task 每次 audio_service_read 的帧数（双麦，2 通道） */
#define APS_READ_FRAMES          256
#define APS_TASK_STOP_TIMEOUT_MS 1200

typedef struct
{
    /* AFE 句柄 */
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t        *afe_data;
    srmodel_list_t           *models;
    int                       feed_chunksize; /* 单通道样本数 */

    /* 重采样器（24kHz->16kHz, 2通道） */
    esp_ae_rate_cvt_handle_t rate_cvt;

    audio_processor_service_state_t state;
    audio_processor_capture_state_t capture_state;
    esp_err_t                       last_error;

    /* 输出 */
    int16_t  *out_buf;
    size_t    out_cap;
    size_t    out_written;
    bool      capture_has_speech;
    uint32_t  capture_speech_ms;
    uint32_t  capture_silence_ms;
    size_t    capture_discard_samples;

    /* feed 侧预分配缓冲 */
    int16_t *cvt_out;      /* 重采样输出 */
    int      cvt_out_cap;  /* 样本数 */
    int16_t *resample_buf; /* 累积重采样后的双麦 16kHz */
    int      resample_buf_cap;
    int      resample_fill;
    int16_t *feed_chunk; /* AFE feed 所需的一块 */
    int16_t *read_buf;   /* feed_task 的 audio_service_read 缓冲（内部 RAM） */
} aps_ctx_t;

static aps_ctx_t s_ctx;
static SemaphoreHandle_t s_control_lock;
static SemaphoreHandle_t s_capture_lock;

static void feed_task_step(void);
static bool fetch_task_step(bool draining);

/**
 * @brief 释放已经停止执行的 AFE、模型、重采样和缓冲资源
 */
static void release_processing_resources(void)
{
    if (s_ctx.afe_data != NULL && s_ctx.afe_iface != NULL)
    {
        s_ctx.afe_iface->destroy(s_ctx.afe_data);
    }
    if (s_ctx.rate_cvt != NULL)
    {
        esp_ae_rate_cvt_close(s_ctx.rate_cvt);
    }
    free(s_ctx.read_buf);
    free(s_ctx.feed_chunk);
    free(s_ctx.resample_buf);
    free(s_ctx.cvt_out);
    if (s_ctx.models != NULL)
    {
        esp_srmodel_deinit(s_ctx.models);
    }
    s_ctx = (aps_ctx_t) { 0 };
}

/** @brief 回滚尚未发布成功的 AFE 初始化资源。 */
static void cleanup_init_failure(void)
{
    (void) audio_processor_task_runtime_deinit(APS_TASK_STOP_TIMEOUT_MS);
    release_processing_resources();
    if (s_capture_lock != NULL)
    {
        vSemaphoreDelete(s_capture_lock);
        s_capture_lock = NULL;
    }
    if (s_control_lock != NULL)
    {
        vSemaphoreDelete(s_control_lock);
        s_control_lock = NULL;
    }
}

/* ── 初始化 AFE ──────────────────────────────────────── */

esp_err_t audio_processor_service_init(void)
{
    if (s_control_lock != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(audio_service_is_initialized(), ESP_ERR_INVALID_STATE, TAG, "音频 Service 尚未初始化");

    s_control_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_control_lock != NULL, ESP_ERR_NO_MEM, TAG, "创建 AFE 生命周期互斥锁失败");
    s_capture_lock = xSemaphoreCreateMutex();
    if (s_capture_lock == NULL)
    {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t task_runtime_error = audio_processor_task_runtime_init(feed_task_step, fetch_task_step);
    if (task_runtime_error != ESP_OK)
    {
        cleanup_init_failure();
        return task_runtime_error;
    }

    /* 加载模型：从 model 分区 mmap（需要分区表中有 model 分区 + srmodels.bin） */
    s_ctx.models = esp_srmodel_init("model");
    if (s_ctx.models == NULL)
    {
        ESP_LOGW(TAG, "未找到 model 分区，AFE 将不加载 NS/VAD 模型（仅基础降噪）");
    }
    char *ns_model_name   = esp_srmodel_filter(s_ctx.models, ESP_NSNET_PREFIX, NULL);
    char *vad_model_name  = esp_srmodel_filter(s_ctx.models, ESP_VADN_PREFIX, NULL);

    /* "MM" = 双麦输入，AFE_TYPE_SR 支持多麦 SE(BSS 空间降噪)
     * VC 模式只支持单麦，SE 被禁用，第二麦数据会被丢弃 */
    /* 启用 WakeNet 时，AFE 会从 models 列表中挑选 ESP_WN_PREFIX 模型。 */
    afe_config_t *afe_cfg = afe_config_init("MM", s_ctx.models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_cfg == NULL)
    {
        cleanup_init_failure();
        ESP_LOGE(TAG, "afe_config_init 失败");
        return ESP_FAIL;
    }

    afe_cfg->aec_init = false; /* 按键触发，无回声消除 */
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    /* 启用 WakeNet 常驻唤醒检测；若 models 为空或无 wn 前缀模型则降级（不 crash） */
    char *wn_model_name = esp_srmodel_filter(s_ctx.models, ESP_WN_PREFIX, NULL);
    if (wn_model_name != NULL)
    {
        afe_cfg->wakenet_init = true;
        ESP_LOGI(TAG, "WakeNet 模型: %s", wn_model_name);
    }
    else
    {
        afe_cfg->wakenet_init = false;
        ESP_LOGW(TAG, "未找到 WakeNet 模型，唤醒检测降级关闭（检查 sdkconfig CONFIG_SR_WN_*）");
    }
#else
    afe_cfg->wakenet_init = false;
#endif
    afe_cfg->se_init          = true; /* 启用双麦空间增强(BSS)，盲源分离抑制非主方向噪声 */
    afe_cfg->vad_init         = true;
    afe_cfg->vad_mode         = VAD_MODE_0;
    afe_cfg->vad_min_noise_ms = 100;
    if (vad_model_name != NULL)
    {
        afe_cfg->vad_model_name = vad_model_name;
    }

    if (ns_model_name != NULL)
    {
        afe_cfg->ns_init       = true;
        afe_cfg->ns_model_name = ns_model_name;
        /* NSNET2 NET 模式，SE 未激活时作为后备降噪 */
        afe_cfg->afe_ns_mode   = AFE_NS_MODE_NET;
    }
    else
    {
        ESP_LOGW(TAG, "未找到 NS 模型，仅使用基础 AFE 降噪");
        afe_cfg->ns_init = false;
    }

    /* AGC 关闭，与 xiaozhi 项目一致；硬件增益已设 30dB */
    afe_cfg->agc_init          = false;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_ctx.afe_iface            = esp_afe_handle_from_config(afe_cfg);
    if (s_ctx.afe_iface == NULL)
    {
        afe_config_free(afe_cfg);
        cleanup_init_failure();
        ESP_LOGE(TAG, "获取 AFE 接口失败");
        return ESP_FAIL;
    }
    s_ctx.afe_data = s_ctx.afe_iface->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    if (s_ctx.afe_data == NULL)
    {
        cleanup_init_failure();
        ESP_LOGE(TAG, "AFE 创建失败");
        return ESP_FAIL;
    }

    s_ctx.feed_chunksize          = s_ctx.afe_iface->get_feed_chunksize(s_ctx.afe_data);
    int fetch_cs                  = s_ctx.afe_iface->get_fetch_chunksize(s_ctx.afe_data);

    /* 重采样器：24kHz -> 16kHz, 双麦通道 */
    esp_ae_rate_cvt_cfg_t cvt_cfg = {
        .src_rate        = APS_HW_SAMPLE_RATE,
        .dest_rate       = APS_AFE_SAMPLE_RATE,
        .channel         = APS_AFE_MIC_CHANNELS,
        .bits_per_sample = ESP_AE_BIT16,
        .complexity      = 2,
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    const esp_ae_err_t rate_error = esp_ae_rate_cvt_open(&cvt_cfg, &s_ctx.rate_cvt);
    if (rate_error != ESP_AE_ERR_OK)
    {
        ESP_LOGE(TAG, "创建重采样器失败: %d", (int) rate_error);
        cleanup_init_failure();
        return ESP_FAIL;
    }

    /* 预分配 feed 侧缓冲（内部 RAM，热路径需低延迟） */
    int need_per_feed      = s_ctx.feed_chunksize * APS_AFE_MIC_CHANNELS;
    /* resample_buf 需能容纳多个 feed 块的累积 */
    s_ctx.resample_buf_cap = need_per_feed * 4;
    s_ctx.resample_buf     = heap_caps_malloc(s_ctx.resample_buf_cap * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    s_ctx.feed_chunk       = heap_caps_malloc(need_per_feed * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    /* 重采样输出缓冲 */
    s_ctx.cvt_out_cap      = 512 * APS_AFE_MIC_CHANNELS;
    s_ctx.cvt_out          = heap_caps_malloc(s_ctx.cvt_out_cap * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    /* feed_task 的 audio_service_read 缓冲（内部 RAM） */
    s_ctx.read_buf         = heap_caps_malloc(APS_READ_FRAMES * APS_HW_CHANNELS * sizeof(int16_t), MALLOC_CAP_INTERNAL);

    if (s_ctx.resample_buf == NULL || s_ctx.feed_chunk == NULL || s_ctx.cvt_out == NULL || s_ctx.read_buf == NULL)
    {
        ESP_LOGE(TAG, "分配中间缓冲失败");
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }

    s_ctx.state         = AUDIO_PROCESSOR_STATE_STOPPED;
    s_ctx.capture_state = AUDIO_PROCESSOR_CAPTURE_IDLE;
    s_ctx.last_error    = ESP_OK;
    ESP_LOGI(TAG,
              "AFE 初始化完成(SR+双麦SE+WakeNet=%s): feed=%d fetch=%d，Runtime 保持停止",
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
              "on",
#else
              "off",
#endif
              s_ctx.feed_chunksize,
              fetch_cs);
    return ESP_OK;
}

esp_err_t audio_processor_service_deinit(void)
{
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    if (s_ctx.state != AUDIO_PROCESSOR_STATE_STOPPED)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t task_error = audio_processor_task_runtime_deinit(APS_TASK_STOP_TIMEOUT_MS);
    if (task_error != ESP_OK)
    {
        s_ctx.state      = AUDIO_PROCESSOR_STATE_CLEANUP_FAILED;
        s_ctx.last_error = task_error;
        xSemaphoreGive(s_control_lock);
        return task_error;
    }
    release_processing_resources();
    xSemaphoreGive(s_control_lock);
    vSemaphoreDelete(s_capture_lock);
    s_capture_lock = NULL;
    vSemaphoreDelete(s_control_lock);
    s_control_lock = NULL;
    ESP_LOGI(TAG, "AFE 音频处理 Service 已反初始化");
    return ESP_OK;
}

bool audio_processor_service_is_initialized(void)
{
    return s_control_lock != NULL;
}

esp_err_t audio_processor_service_start(void)
{
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    if (s_ctx.state == AUDIO_PROCESSOR_STATE_RUNNING)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_OK;
    }
    if (s_ctx.state != AUDIO_PROCESSOR_STATE_STOPPED)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.state      = AUDIO_PROCESSOR_STATE_RUNNING;
    s_ctx.last_error = ESP_OK;
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    const esp_err_t task_error = audio_processor_task_runtime_ensure_created();
    if (task_error != ESP_OK)
    {
        s_ctx.state      = AUDIO_PROCESSOR_STATE_STOPPED;
        s_ctx.last_error = task_error;
        xSemaphoreGive(s_control_lock);
        return task_error;
    }
    audio_processor_task_runtime_begin_processing();
#endif
    xSemaphoreGive(s_control_lock);
    ESP_LOGI(TAG, "AFE Runtime 已启动，等待采集请求");
    return ESP_OK;
}

esp_err_t audio_processor_service_stop(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "AFE Runtime 停止超时无效");
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    if (s_ctx.state == AUDIO_PROCESSOR_STATE_STOPPED)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_OK;
    }
    if (s_ctx.state != AUDIO_PROCESSOR_STATE_RUNNING
        && s_ctx.state != AUDIO_PROCESSOR_STATE_CLEANUP_FAILED)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    const bool capture_idle = s_ctx.capture_state == AUDIO_PROCESSOR_CAPTURE_IDLE;
    xSemaphoreGive(s_capture_lock);
    if (!capture_idle)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.state = AUDIO_PROCESSOR_STATE_STOPPING;
    const esp_err_t error = audio_processor_task_runtime_park(timeout_ms);
    if (error == ESP_OK)
    {
        s_ctx.state      = AUDIO_PROCESSOR_STATE_STOPPED;
        s_ctx.last_error = ESP_OK;
    }
    else
    {
        s_ctx.state      = AUDIO_PROCESSOR_STATE_CLEANUP_FAILED;
        s_ctx.last_error = error;
    }
    xSemaphoreGive(s_control_lock);
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG, "AFE Runtime 已停止，处理 Task 已停泊");
    }
    else
    {
        ESP_LOGE(TAG, "AFE Runtime 停止失败: %s", esp_err_to_name(error));
    }
    return error;
}

esp_err_t audio_processor_service_get_status_copy(audio_processor_service_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "AFE 状态输出为空");
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    audio_processor_task_status_t task_status = { 0 };
    audio_processor_task_runtime_get_status(&task_status);
    *out_status = (audio_processor_service_status_t) {
        .state         = s_ctx.state,
        .capture_state = s_ctx.capture_state,
        .tasks_created = task_status.tasks_created,
        .feed_parked   = task_status.feed_parked,
        .fetch_parked  = task_status.fetch_parked,
        .last_error    = s_ctx.last_error,
    };
    xSemaphoreGive(s_capture_lock);
    xSemaphoreGive(s_control_lock);
    return ESP_OK;
}

/* ── 进入收集模式（默认配置首次启动任务，后续会话复用）── */

esp_err_t audio_processor_service_capture_start(int16_t *out_buf, size_t out_cap)
{
    if (s_control_lock == NULL)
    {
        ESP_LOGE(TAG, "AFE 未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    if (out_buf == NULL || out_cap == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    if (s_ctx.state != AUDIO_PROCESSOR_STATE_RUNNING)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (s_ctx.capture_state != AUDIO_PROCESSOR_CAPTURE_IDLE)
    {
        xSemaphoreGive(s_capture_lock);
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.out_buf                 = out_buf;
    s_ctx.out_cap                 = out_cap;
    s_ctx.out_written             = 0;
    s_ctx.capture_has_speech      = false;
    s_ctx.capture_speech_ms       = 0;
    s_ctx.capture_silence_ms      = 0;
    s_ctx.capture_discard_samples = 0;
    s_ctx.resample_fill           = 0;
    s_ctx.capture_state           = AUDIO_PROCESSOR_CAPTURE_CAPTURING;
    xSemaphoreGive(s_capture_lock);

    audio_processor_task_status_t task_status = { 0 };
    audio_processor_task_runtime_get_status(&task_status);
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    /* WakeNet 常驻 feed/fetch，保留既有的会话起点清理语义。 */
    s_ctx.afe_iface->reset_buffer(s_ctx.afe_data);
#else
    /* 默认配置在首轮任务启动前清理一次。后续会话由上一轮 drain 保证管线为空，
     * 避免在常驻 fetch 正等待结果时并发 reset AFE 内部 ringbuffer。 */
    if (!task_status.tasks_created)
    {
        s_ctx.afe_iface->reset_buffer(s_ctx.afe_data);
    }
#endif

    const esp_err_t task_error = audio_processor_task_runtime_ensure_created();
    if (task_error != ESP_OK)
    {
        xSemaphoreTake(s_capture_lock, portMAX_DELAY);
        s_ctx.capture_state = AUDIO_PROCESSOR_CAPTURE_IDLE;
        s_ctx.out_buf       = NULL;
        s_ctx.out_cap       = 0U;
        xSemaphoreGive(s_capture_lock);
        s_ctx.last_error = task_error;
        xSemaphoreGive(s_control_lock);
        return task_error;
    }
    audio_processor_task_runtime_begin_processing();
    s_ctx.last_error = ESP_OK;
    xSemaphoreGive(s_control_lock);

    ESP_LOGI(TAG, "降噪收集开始 (out_cap=%u)", (unsigned) out_cap);
    return ESP_OK;
}

/* ── 内部：双麦 24kHz -> 重采样 16kHz -> 喂 AFE（feed_task 与原外部 feed 共用）── */

static void process_mic_chunk(int16_t *data, int num_samples)
{
    _Static_assert(APS_HW_CHANNELS == APS_AFE_MIC_CHANNELS,
                   "dual-mic deinterleave removed: requires equal HW/AFE channel count");
    if (s_ctx.afe_data == NULL || data == NULL || num_samples <= 0)
    {
        return;
    }
    int frames = num_samples / APS_HW_CHANNELS;
    if (frames == 0)
    {
        return;
    }

    /* HW 与 AFE 通道数相等（见上方 _Static_assert），data 已是双通道交错布局，
     * 直接喂给重采样器，无需去交错拷贝。 */
    uint32_t in_samples = frames;
    uint32_t max_out    = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(s_ctx.rate_cvt, in_samples, &max_out);
    if (max_out * APS_AFE_MIC_CHANNELS > (uint32_t) s_ctx.cvt_out_cap)
    {
        ESP_LOGW(TAG, "重采样输出超限: %lu", (unsigned long) max_out);
        return;
    }
    uint32_t actual_out = max_out;
    esp_ae_rate_cvt_process(s_ctx.rate_cvt, data, in_samples, s_ctx.cvt_out, &actual_out);
    int cvt_total     = (int) actual_out * APS_AFE_MIC_CHANNELS;

    int need_per_feed = s_ctx.feed_chunksize * APS_AFE_MIC_CHANNELS;
    int src_pos       = 0;
    while (src_pos < cvt_total)
    {
        int space = s_ctx.resample_buf_cap - s_ctx.resample_fill;
        int take  = (cvt_total - src_pos) < space ? (cvt_total - src_pos) : space;
        memcpy(s_ctx.resample_buf + s_ctx.resample_fill, s_ctx.cvt_out + src_pos, take * sizeof(int16_t));
        s_ctx.resample_fill += take;
        src_pos += take;

        while (s_ctx.resample_fill >= need_per_feed)
        {
            memcpy(s_ctx.feed_chunk, s_ctx.resample_buf, need_per_feed * sizeof(int16_t));
            int remain = s_ctx.resample_fill - need_per_feed;
            if (remain > 0)
            {
                memmove(s_ctx.resample_buf, s_ctx.resample_buf + need_per_feed, remain * sizeof(int16_t));
            }
            s_ctx.resample_fill = remain;
            s_ctx.afe_iface->feed(s_ctx.afe_data, s_ctx.feed_chunk);
        }
    }
}

/** @brief feed Task 单步读取麦克风并送入重采样和 AFE。 */
static void feed_task_step(void)
{
    const int read_samples = APS_READ_FRAMES * APS_HW_CHANNELS;
    size_t    got          = 0U;
    const esp_err_t read_error = audio_service_read(s_ctx.read_buf, (size_t) read_samples, &got);
    if (read_error == ESP_OK && got > 0U)
    {
        process_mic_chunk(s_ctx.read_buf, (int) got);
        return;
    }

    /* 活动采集期间的短暂 I2S 抖动用静音维持管线，避免错误路径忙循环。 */
    memset(s_ctx.read_buf, 0, read_samples * sizeof(int16_t));
    process_mic_chunk(s_ctx.read_buf, read_samples);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/**
 * @brief fetch Task 单步提取 AFE 结果
 *
 * @param[in] draining 是否已请求排空
 * @return true drain 已完成，Task Runtime 应停止 fetch；false 继续处理
 */
static bool fetch_task_step(bool draining)
{
    static TickType_t last_failure_log;
    afe_fetch_result_t *res =
        s_ctx.afe_iface->fetch_with_delay(s_ctx.afe_data, pdMS_TO_TICKS(APS_FETCH_WAIT_MS));
    if (res == NULL || res->ret_value == ESP_FAIL)
    {
        if (draining)
        {
            return true;
        }
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_failure_log) >= pdMS_TO_TICKS(1000))
        {
            ESP_LOGW(TAG, "AFE fetch 暂无结果: ret=%d", res == NULL ? -1 : res->ret_value);
            last_failure_log = now;
        }
        vTaskDelay(1);
        return false;
    }

    bool request_drain = false;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (s_ctx.capture_state == AUDIO_PROCESSOR_CAPTURE_CAPTURING
        || s_ctx.capture_state == AUDIO_PROCESSOR_CAPTURE_DRAINING)
    {
        const uint32_t frame_ms =
            (uint32_t) (res->data_size * 1000 / sizeof(int16_t) / APS_AFE_SAMPLE_RATE);
        if (res->vad_state == VAD_SPEECH)
        {
            if (!s_ctx.capture_has_speech)
            {
                ESP_LOGI(TAG, "VAD 检测到人声，开始有效录音");
            }
            s_ctx.capture_has_speech = true;
            s_ctx.capture_speech_ms += frame_ms;
            s_ctx.capture_silence_ms = 0;
        }
        else if (s_ctx.capture_has_speech)
        {
            s_ctx.capture_silence_ms += frame_ms;
        }

        const size_t samples = (size_t) res->data_size / sizeof(int16_t);
        if (s_ctx.out_buf != NULL && s_ctx.out_written + samples <= s_ctx.out_cap)
        {
            memcpy(s_ctx.out_buf + s_ctx.out_written, res->data, res->data_size);
            s_ctx.out_written += samples;
        }
        else if (s_ctx.capture_state == AUDIO_PROCESSOR_CAPTURE_CAPTURING)
        {
            s_ctx.capture_state = AUDIO_PROCESSOR_CAPTURE_DRAINING;
            request_drain       = true;
            ESP_LOGW(TAG, "输出缓冲已满 (%u 样本)，开始排空", (unsigned) s_ctx.out_written);
        }
    }
#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    else if (!draining && res->wakeup_state == WAKENET_DETECTED)
    {
        ESP_LOGI(TAG, "唤醒词命中 (model_index=%d)", res->wakenet_model_index);
        const esp_err_t event_error =
            esp_event_post(AUDIO_PROCESSOR_EVENT, AUDIO_PROCESSOR_EVENT_WAKE, NULL, 0, 0);
        if (event_error != ESP_OK)
        {
            ESP_LOGW(TAG, "投递唤醒事件失败: %s", esp_err_to_name(event_error));
        }
    }
#endif
    xSemaphoreGive(s_capture_lock);

    if (request_drain)
    {
        audio_processor_task_runtime_begin_drain();
    }
    return false;
}

/* ── 结束收集：drain 管线残余并等待 Task 停泊 ────────── */

esp_err_t audio_processor_service_capture_stop(size_t *out_sample_count)
{
    ESP_RETURN_ON_FALSE(out_sample_count != NULL, ESP_ERR_INVALID_ARG, TAG, "降噪样本数输出指针为空");
    *out_sample_count = 0U;
    if (s_control_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_control_lock, portMAX_DELAY);
    if (s_ctx.state != AUDIO_PROCESSOR_STATE_RUNNING)
    {
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (s_ctx.capture_state == AUDIO_PROCESSOR_CAPTURE_IDLE)
    {
        xSemaphoreGive(s_capture_lock);
        xSemaphoreGive(s_control_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool begin_drain = s_ctx.capture_state == AUDIO_PROCESSOR_CAPTURE_CAPTURING;
    s_ctx.capture_state    = AUDIO_PROCESSOR_CAPTURE_DRAINING;
    xSemaphoreGive(s_capture_lock);

    if (begin_drain)
    {
        ESP_LOGI(TAG, "停止 AFE feed，开始排空残余管线");
        audio_processor_task_runtime_begin_drain();
    }
    const esp_err_t drain_error =
        audio_processor_task_runtime_wait_drain_and_park(CONFIG_DESKMATE_AUDIO_DRAIN_MAX_WAIT_MS);
    if (drain_error != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "AFE drain 或 Task 停泊超时: %u ms",
                 (unsigned) CONFIG_DESKMATE_AUDIO_DRAIN_MAX_WAIT_MS);
    }

    /* fetch 每次写输出都持有同一把锁；取得锁后可以安全撤销调用方缓冲借用。 */
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    size_t discard = s_ctx.capture_discard_samples;
    if (discard > s_ctx.out_written)
    {
        discard = s_ctx.out_written;
    }
    if (discard > 0)
    {
        size_t kept = s_ctx.out_written - discard;
        memmove(s_ctx.out_buf, s_ctx.out_buf + discard, kept * sizeof(int16_t));
        s_ctx.out_written = kept;
        ESP_LOGI(TAG, "已裁剪唤醒保护窗音频: %u 样本", (unsigned) discard);
    }

    ESP_LOGI(TAG,
             "降噪收集结束: 输出 %u 样本 (%.1f 秒)",
             (unsigned) s_ctx.out_written,
             (float) s_ctx.out_written / APS_AFE_SAMPLE_RATE);
    *out_sample_count      = s_ctx.out_written;
    s_ctx.capture_state    = AUDIO_PROCESSOR_CAPTURE_IDLE;
    s_ctx.out_buf          = NULL;
    s_ctx.out_cap          = 0U;
    xSemaphoreGive(s_capture_lock);

#if CONFIG_DESKMATE_WAKE_WORD_ENABLE
    if (drain_error == ESP_OK)
    {
        audio_processor_task_runtime_begin_processing();
    }
#endif
    s_ctx.last_error = drain_error;
    xSemaphoreGive(s_control_lock);
    return drain_error;
}

bool audio_processor_service_capture_has_speech(void)
{
    if (s_capture_lock == NULL)
    {
        return false;
    }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    const bool has_speech =
        s_ctx.capture_has_speech && s_ctx.capture_speech_ms >= CONFIG_DESKMATE_AUDIO_SPEECH_MIN_MS;
    xSemaphoreGive(s_capture_lock);
    return has_speech;
}

uint32_t audio_processor_service_capture_silence_ms(void)
{
    if (s_capture_lock == NULL)
    {
        return 0U;
    }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    const uint32_t silence_ms = s_ctx.capture_silence_ms;
    xSemaphoreGive(s_capture_lock);
    return silence_ms;
}

esp_err_t audio_processor_service_capture_reset_activity(void)
{
    if (s_capture_lock == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    if (s_ctx.capture_state != AUDIO_PROCESSOR_CAPTURE_CAPTURING)
    {
        xSemaphoreGive(s_capture_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.capture_has_speech      = false;
    s_ctx.capture_speech_ms       = 0;
    s_ctx.capture_silence_ms      = 0;
    s_ctx.capture_discard_samples = s_ctx.out_written;
    ESP_LOGI(TAG, "已清除唤醒尾音 VAD 标记，待裁剪 %u 样本", (unsigned) s_ctx.capture_discard_samples);
    xSemaphoreGive(s_capture_lock);
    return ESP_OK;
}
