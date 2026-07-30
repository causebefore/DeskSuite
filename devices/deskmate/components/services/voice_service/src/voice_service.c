/*
 * 文件职责：封装录音 + 流式播放的语音交互闭环。
 * 主要依赖：audio_service、transport、voice_protocol、settings_store。
 * 调用方：App 业务流程（按键或唤醒词触发）。
 *
 * 数据流：
 *   录音：(MIC) → ES7210 2通道 → audio_processor_service (双麦 AFE 降噪)
 *         → 单声道 16kHz PCM → WebSocket 上传（失败时回退 HTTP）
 *   播放：server 流式帧响应 → 逐帧解析 → TTS_PCM 帧 → 边收边播
 */
#include "voice_service.h"
#include "voice_service_internal.h"

#include <stdlib.h>
#include <string.h>

#include "audio_service.h"
#include "audio_processor_service.h"
#include "transport_http.h"
#include "transport_websocket.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "protocol_identity.h"
#include "protocol_url.h"
#include "voice_protocol.h"

static const char *TAG = "voice_service";

#define VOICE_SAMPLE_RATE   16000 /* AFE 降噪后输出采样率 */
/* 单个 int16 样本 */
#define VOICE_SAMPLE_SIZE   2

/* HTTP/VAD/WebSocket 超时与缓冲、TTS 播放 ring 见 Kconfig: DeskMate Audio/Voice。 */

/* HTTP 流式读取缓冲 */
#define VOICE_HTTP_READ_BUF 2048

ESP_EVENT_DEFINE_BASE(VOICE_SERVICE_EVENT);

typedef struct
{
    voice_service_state_t state;
    bool                  busy;
    esp_err_t             last_error;
} voice_service_ctx_t;

/**
 * @brief 单轮语音会话使用的服务配置和稳定设备身份快照
 */
typedef struct
{
    protocol_backend_context_t backend; /*!< 本轮完整后端上下文 */
} voice_session_config_t;

static voice_service_ctx_t s_ctx;
static StaticEventGroup_t  s_session_events_storage;
static EventGroupHandle_t  s_session_events;
static SemaphoreHandle_t   s_chat_stopped;
static portMUX_TYPE        s_session_lock = portMUX_INITIALIZER_UNLOCKED;

#define VOICE_SESSION_CANCELLED BIT0

/* 语音对话用的 server 配置快照。必须在 voice_service_request_chat（内部 RAM 栈任务）里
 * 预加载——voice_chat_task 的栈在 PSRAM，不能执行 NVS/flash 读（会 disable cache，
 * PSRAM 栈不可访问，触发 esp_task_stack_is_sane_cache_disabled 断言）。 */
static voice_session_config_t s_chat_config;

/* 播放 ring（生产/消费解耦）：on_stream_frame 收到的 24kHz PCM 写入此 ring，
 * 独立 playback_task 按 DAC 速率消费喂 DMA。ring 用 PSRAM，256KB≈5 秒@24kHz，
 * 用于吸收服务端 TTS 按句突发下发（生成速率远超 1× 播放）；再叠加接收队列的丢包
 * 兜底，避免 DAC underrun 爆音与会话中断。voice_service_init 创建，跨对话复用，
 * 每次对话前 reset。 */
/* TTS 播放 ring 缓冲大小见 Kconfig: DeskMate Audio/Voice */
static StaticStreamBuffer_t s_play_stream_struct;
static uint8_t             *s_play_stream_buf; /* PSRAM，init 分配 */
static StreamBufferHandle_t s_play_stream;     /* init 创建，复用 */

/* 流式播放上下文，在帧回调中更新状态 */
typedef struct
{
    volatile bool speaking_started;
    volatile bool got_end;
    volatile bool got_error;
    bool          received_frame;
    /* ── 播放统计（用于会话结束汇总日志）── */
    int64_t tts_start_us;    /* 首帧 TTS 到达时刻 */
    size_t  tts_samples_out; /* 累计输出 24kHz 样本数 */
    int     tts_frame_cnt;   /* TTS 帧计数 */
    /* ── 播放消费任务（与 on_stream_frame 生产解耦）── */
    bool              playback_started;
    volatile bool     playback_finishing; /* voice_http_stream 结束，让任务 drain 后退出 */
    volatile bool     playback_discarding;
    SemaphoreHandle_t playback_done;
} stream_ctx_t;

static esp_err_t voice_ws_stream(const uint8_t *upload, size_t upload_len);

static void publish(voice_service_event_t ev)
{
    esp_err_t err = esp_event_post(VOICE_SERVICE_EVENT, ev, NULL, 0, pdMS_TO_TICKS(100));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "投递语音事件失败: id=%d, err=%s", (int) ev, esp_err_to_name(err));
    }
}

static bool session_try_activate(void)
{
    bool activated = false;
    portENTER_CRITICAL(&s_session_lock);
    if (s_ctx.state == VOICE_SERVICE_STATE_RUNNING && !s_ctx.busy)
    {
        s_ctx.busy = true;
        activated  = true;
    }
    portEXIT_CRITICAL(&s_session_lock);
    return activated;
}

static void session_set_idle(void)
{
    portENTER_CRITICAL(&s_session_lock);
    s_ctx.busy = false;
    portEXIT_CRITICAL(&s_session_lock);
}

static bool session_is_busy(void)
{
    portENTER_CRITICAL(&s_session_lock);
    bool busy = s_ctx.busy;
    portEXIT_CRITICAL(&s_session_lock);
    return busy;
}

/* ── 流式帧回调 ──────────────────────────────────────── */

static void run_playback(void *arg)
{
    stream_ctx_t *ctx = (stream_ctx_t *) arg;
    int16_t       buf[480]; /* 20ms @24kHz，小批喂 DMA */
    size_t        total_samples   = 0;
    size_t        nonzero_samples = 0;
    uint64_t      sum_abs_samples = 0;
    uint32_t      peak_sample     = 0;
    for (;;)
    {
        if (ctx->playback_discarding)
        {
            xStreamBufferReset(s_play_stream);
            break;
        }
        size_t n = xStreamBufferReceive(s_play_stream, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0)
        {
            const size_t    sample_count = n / VOICE_SAMPLE_SIZE;
            size_t          written      = 0U;
            const esp_err_t write_error  = audio_service_write((const int16_t *) buf, sample_count, &written);
            if (write_error != ESP_OK || written != sample_count)
            {
                ESP_LOGE(TAG,
                         "播放写入失败: expected=%u actual=%u err=%s",
                         (unsigned) sample_count,
                         (unsigned) written,
                         esp_err_to_name(write_error));
                ctx->got_error = true;
                xStreamBufferReset(s_play_stream);
                break;
            }
            for (size_t index = 0; index < written; ++index)
            {
                int32_t magnitude = buf[index];
                if (magnitude < 0)
                {
                    magnitude = -magnitude;
                }
                if ((uint32_t) magnitude > peak_sample)
                {
                    peak_sample = (uint32_t) magnitude;
                }
                if (buf[index] != 0)
                {
                    ++nonzero_samples;
                }
                sum_abs_samples += (uint32_t) magnitude;
            }
            total_samples += (size_t) written;
        }
        else if (ctx->playback_finishing && xStreamBufferBytesAvailable(s_play_stream) == 0)
        {
            break; /* 收尾且 ring 已排空 */
        }
    }
    const unsigned mean_abs_sample = total_samples > 0U ? (unsigned) (sum_abs_samples / total_samples) : 0U;
    if (total_samples > 0U && peak_sample == 0U)
    {
        ESP_LOGW(TAG, "播放任务退出: PCM 全为零, samples=%u error=%d", (unsigned) total_samples, (int) ctx->got_error);
    }
    else
    {
        ESP_LOGI(TAG,
                 "播放任务退出: samples=%u nonzero=%u peak=%u mean_abs=%u error=%d",
                 (unsigned) total_samples,
                 (unsigned) nonzero_samples,
                 (unsigned) peak_sample,
                 mean_abs_sample,
                 (int) ctx->got_error);
    }
    (void) audio_service_enable_output(false);
    if (ctx->playback_done != NULL)
    {
        xSemaphoreGive(ctx->playback_done);
    }
}

static bool session_cancelled(void)
{
    return s_session_events != NULL && (xEventGroupGetBits(s_session_events) & VOICE_SESSION_CANCELLED) != 0;
}

static void finish_chat_task(voice_service_event_t terminal_event)
{
    /* 先把终态排进事件队列，再释放 busy。这样下一轮 RECORDING 不会先于
     * 上一轮 DONE/ERROR/CANCELLED 到达 UI，避免界面被旧终态覆盖。 */
    esp_err_t post_err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3 && post_err != ESP_OK; ++attempt)
    {
        post_err = esp_event_post(VOICE_SERVICE_EVENT, terminal_event, NULL, 0, pdMS_TO_TICKS(250));
        if (post_err != ESP_OK)
        {
            ESP_LOGW(TAG,
                     "终态事件投递失败: id=%d attempt=%d err=%s",
                     (int) terminal_event,
                     attempt,
                     esp_err_to_name(post_err));
        }
    }
    session_set_idle();
    if (s_chat_stopped != NULL)
    {
        (void) xSemaphoreGive(s_chat_stopped);
    }
}

/**
 * @brief 创建本轮播放消费任务并使能音频输出
 *
 * 播放 Task 栈固定从 PSRAM 分配，避免 WebSocket Client 的 Task、队列和协议缓冲已经创建后，
 * 内部堆峰值导致第二个 Task 创建失败。任务退出必须与 xTaskCreateWithCaps 配对调用
 * vTaskDeleteWithCaps。
 *
 * @param[in,out] ctx 本轮流式播放上下文
 * @return ESP_OK 播放任务已启动；ESP_ERR_NO_MEM 资源不足；其他值表示音频输出使能失败
 */
static esp_err_t playback_start(stream_ctx_t *ctx)
{
    ctx->playback_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(ctx->playback_done != NULL, ESP_ERR_NO_MEM, TAG, "创建播放完成信号失败");
    esp_err_t err = audio_service_enable_output(true);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(ctx->playback_done);
        ctx->playback_done = NULL;
        ESP_LOGE(TAG, "使能播放失败: %s", esp_err_to_name(err));
        return err;
    }
    xStreamBufferReset(s_play_stream);
    const esp_err_t task_error = voice_service_task_start_playback(run_playback, ctx);
    if (task_error != ESP_OK)
    {
        (void) audio_service_enable_output(false);
        vSemaphoreDelete(ctx->playback_done);
        ctx->playback_done = NULL;
        ESP_LOGE(TAG,
                 "创建播放任务失败: 内部堆=%u PSRAM=%u",
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return task_error;
    }
    ctx->playback_started = true;
    ESP_LOGI(TAG, "播放任务已启动: ring=%u bytes", (unsigned) CONFIG_DESKMATE_VOICE_PLAY_STREAM_BYTES);
    return ESP_OK;
}

static esp_err_t playback_finish(stream_ctx_t *ctx, bool discard)
{
    if (!ctx->playback_started)
    {
        (void) audio_service_enable_output(false);
        if (ctx->playback_done != NULL)
        {
            vSemaphoreDelete(ctx->playback_done);
            ctx->playback_done = NULL;
        }
        return ESP_OK;
    }
    if (discard)
    {
        ESP_LOGW(TAG, "丢弃播放缓冲: queued=%u bytes", (unsigned) xStreamBufferBytesAvailable(s_play_stream));
        ctx->playback_discarding = true;
    }
    ctx->playback_finishing  = true;

    /* 正常结束需把播放 ring 里的积压音频 drain 完：按当前积压量估算最长 drain
     * 时间（+1s 余量）作为等待上限，避免 ring 扩容后固定超时把尚未播完的正常
     * 音频强制掐断。discard 路径下 playback_task 检测 discarding 会立即退出，
     * 用基础超时即可，不受此动态上限影响。 */
    uint32_t stop_timeout_ms = CONFIG_DESKMATE_VOICE_PLAYBACK_STOP_TIMEOUT_MS;
    if (!discard)
    {
        size_t   queued = xStreamBufferBytesAvailable(s_play_stream);
        uint32_t drain_ms =
            (uint32_t) ((uint64_t) queued * 1000 / (audio_service_get_sample_rate_hz() * VOICE_SAMPLE_SIZE)) + 1000;
        if (drain_ms > stop_timeout_ms)
        {
            stop_timeout_ms = drain_ms;
        }
    }
    if (xSemaphoreTake(ctx->playback_done, pdMS_TO_TICKS(stop_timeout_ms)) != pdTRUE)
    {
        ESP_LOGE(TAG, "等待播放任务退出超时(%ums)，关闭输出后继续等待安全退出", (unsigned) stop_timeout_ms);
        (void) audio_service_enable_output(false);
        ctx->got_error = true;
        (void) xSemaphoreTake(ctx->playback_done, portMAX_DELAY);
    }
    ctx->playback_started = false;
    vSemaphoreDelete(ctx->playback_done);
    ctx->playback_done = NULL;
    ESP_LOGI(TAG, "播放任务已完成并关闭输出");
    return ctx->got_error ? ESP_FAIL : ESP_OK;
}

static void on_stream_frame(voice_protocol_frame_type_t type, const uint8_t *payload, size_t len, void *arg)
{
    stream_ctx_t *ctx   = (stream_ctx_t *) arg;
    ctx->received_frame = true;
    switch (type)
    {
        case VOICE_PROTOCOL_FRAME_ASR_TEXT:
            ESP_LOGI(TAG, "ASR 识别: %.*s", (int) len, (const char *) payload);
            break;
        case VOICE_PROTOCOL_FRAME_REPLY_TEXT:
            ESP_LOGI(TAG, "LLM 回复: %.*s", (int) len, (const char *) payload);
            break;
        case VOICE_PROTOCOL_FRAME_TTS_PCM: {
            if (len == 0 || (len % VOICE_SAMPLE_SIZE) != 0)
            {
                ESP_LOGE(TAG, "TTS PCM 帧长度非法: %lu", (unsigned long) len);
                ctx->got_error = true;
                break;
            }
            /* 首个 PCM 分片到达时切换到播放状态 */
            if (!ctx->speaking_started)
            {
                ctx->speaking_started = true;
                ctx->tts_start_us     = esp_timer_get_time();
                ctx->tts_samples_out  = 0;
                ctx->tts_frame_cnt    = 0;
                publish(VOICE_SERVICE_EVENT_SPEAKING);
                ESP_LOGI(TAG, "开始流式播放");
            }

            /* TTS PCM 已是 24kHz 单声道（与硬件输出采样率一致），直接写入播放 ring，
             * 无需重采样。playback_task 按 DAC 速率消费喂 DMA。 */
            int in_samples = (int) (len / VOICE_SAMPLE_SIZE);
            if (in_samples <= 0)
            {
                break;
            }
            size_t sent = xStreamBufferSend(s_play_stream, payload, len, pdMS_TO_TICKS(2000));
            if (sent != len)
            {
                ESP_LOGE(TAG,
                         "TTS 播放缓冲写入超时: expected=%lu actual=%u ring=%u",
                         (unsigned long) len,
                         (unsigned) sent,
                         (unsigned) xStreamBufferBytesAvailable(s_play_stream));
                ctx->got_error = true;
                break;
            }

            ctx->tts_samples_out += in_samples;
            ctx->tts_frame_cnt++;
            break;
        }
        case VOICE_PROTOCOL_FRAME_ERROR:
            ctx->got_error = true;
            ESP_LOGW(TAG, "服务端错误: %.*s", (int) len, (const char *) payload);
            break;
        case VOICE_PROTOCOL_FRAME_END:
            ctx->got_end = true;
            ESP_LOGI(TAG, "收到 END 帧");
            break;
        case VOICE_PROTOCOL_FRAME_THINKING:
            /* 保活帧：服务端正在 ASR 或调用工具，无需处理 */
            break;
        default:
            ESP_LOGW(TAG, "未知帧类型: 0x%02X", type);
            break;
    }
}

/* ── 录音阶段：双麦 AFE 降噪 ─────────────────────────── */

/* 录音阶段：采集双麦数据，经 audio_processor_service（AFE 双麦降噪）
 * 处理后输出单声道 16kHz PCM，存入 buf。
 * 返回录到的字节数。duration_ms 控制录音时长。 */
static size_t record_denoised_pcm(int16_t *buf, size_t buf_samples, uint32_t duration_ms)
{
    /* 默认配置在本会话中启动 feed/fetch；仅启用 WakeNet 时任务和输入会常驻。 */
    esp_err_t err = audio_processor_service_capture_start(buf, buf_samples);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "启动 AFE 收集失败: %s", esp_err_to_name(err));
        return 0;
    }

    /* 忽略唤醒词尾音：继续收集 PCM，但在短保护窗后重置 VAD 活动判定，
     * 后续必须由用户真正说话重新触发人声状态。 */
    vTaskDelay(pdMS_TO_TICKS(CONFIG_DESKMATE_VOICE_VAD_WAKE_GUARD_MS));
    err = audio_processor_service_capture_reset_activity();
    if (err != ESP_OK)
    {
        size_t ignored_samples = 0U;
        (void) audio_processor_service_capture_stop(&ignored_samples);
        ESP_LOGE(TAG, "重置 AFE 会话活动状态失败: %s", esp_err_to_name(err));
        return 0;
    }

    /* 按 duration 计时（feed_task 实时喂、fetch_task 实时收集） */
    uint32_t waited = 0;
    while (waited < duration_ms)
    {
        if (session_cancelled())
        {
            ESP_LOGI(TAG, "录音阶段收到取消请求，提前结束");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (!audio_processor_service_capture_has_speech() && waited >= CONFIG_DESKMATE_VOICE_VAD_LEAD_TIMEOUT_MS)
        {
            ESP_LOGW(TAG, "VAD 前导超时：%u ms 未检测到人声", (unsigned) waited);
            break;
        }
        if (audio_processor_service_capture_has_speech()
            && audio_processor_service_capture_silence_ms() >= CONFIG_DESKMATE_VOICE_VAD_SILENCE_END_MS)
        {
            ESP_LOGI(TAG, "VAD 静音 %u ms，结束录音", (unsigned) audio_processor_service_capture_silence_ms());
            break;
        }
    }

    size_t denoised_samples = 0U;
    err                     = audio_processor_service_capture_stop(&denoised_samples);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "停止 AFE 收集失败: samples=%u err=%s", (unsigned) denoised_samples, esp_err_to_name(err));
        return 0;
    }
    ESP_LOGI(TAG,
             "录音完成: 降噪输出 %u 样本, %d ms",
             (unsigned) denoised_samples,
             (int) (denoised_samples * 1000 / VOICE_SAMPLE_RATE));
    return audio_processor_service_capture_has_speech() ? denoised_samples * VOICE_SAMPLE_SIZE : 0;
}

/* ── 流式上传 + 边收边播 ─────────────────────────────── */

/* 流式上传 PCM 并边收边播 TTS 响应。
 * 上传 raw PCM，接收二进制帧流，逐帧解析：
 * TTS_PCM → 立即写播放，ASR_TEXT/REPLY_TEXT → 日志，END → 结束。
 * 成功返回 ESP_OK。 */
typedef struct
{
    voice_protocol_decoder_t *decoder;
    stream_ctx_t             *stream;
} voice_http_context_t;

static bool voice_http_should_continue(void *arg)
{
    (void) arg;
    return !session_cancelled();
}

static esp_err_t voice_http_on_data(const uint8_t *data, size_t len, void *arg)
{
    voice_http_context_t *context = (voice_http_context_t *) arg;
    const esp_err_t err = voice_protocol_decoder_feed(context->decoder, data, len, on_stream_frame, context->stream);
    if (err != ESP_OK)
    {
        context->stream->got_error = true;
        ESP_LOGE(TAG, "Voice 帧解码失败: %s", esp_err_to_name(err));
        return err;
    }
    return context->stream->got_error ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}

static esp_err_t voice_http_stream(const uint8_t *upload, size_t upload_len)
{
    char url[256] = { 0 };
    ESP_RETURN_ON_ERROR(protocol_url_build(url, sizeof(url), s_chat_config.backend.base_url, "api/v1/voice/chat"),
                        TAG,
                        "构造 Voice HTTP 地址失败");

    char rate_header[16]  = { 0 };
    char auth_header[112] = { 0 };
    snprintf(rate_header, sizeof(rate_header), "%d", VOICE_SAMPLE_RATE);
    transport_http_header_t headers[4] = {
        { .name = "Content-Type",        .value = "audio/pcm" },
        { .name = "X-Audio-Sample-Rate", .value = rate_header },
    };
    size_t header_count = 2U;
    protocol_identity_add_headers(headers,
                                  &header_count,
                                  s_chat_config.backend.token,
                                  s_chat_config.backend.device_id,
                                  auth_header,
                                  sizeof(auth_header));

    voice_protocol_decoder_t *decoder = voice_protocol_decoder_create();
    ESP_RETURN_ON_FALSE(decoder != NULL, ESP_ERR_NO_MEM, TAG, "创建 Voice 帧解码器失败");
    stream_ctx_t stream = { 0 };
    esp_err_t    err    = playback_start(&stream);
    if (err != ESP_OK)
    {
        voice_protocol_decoder_destroy(decoder);
        return err;
    }

    voice_http_context_t context = {
        .decoder = decoder,
        .stream  = &stream,
    };
    const transport_http_stream_request_t request = {
        .url               = url,
        .headers           = headers,
        .header_count      = header_count,
        .upload_data       = upload,
        .upload_len        = upload_len,
        .read_buffer_bytes = VOICE_HTTP_READ_BUF,
        .timeout_ms        = CONFIG_DESKMATE_VOICE_HTTP_TIMEOUT_MS,
        .on_response_data  = voice_http_on_data,
        .should_continue   = voice_http_should_continue,
        .ctx               = &context,
    };
    transport_http_stream_result_t result = { 0 };
    err                                   = transport_http_stream_borrow(&request, &result);

    (void) playback_finish(&stream, session_cancelled() || stream.got_error);
    voice_protocol_decoder_destroy(decoder);

    if (stream.speaking_started)
    {
        const int64_t play_elapsed_us = esp_timer_get_time() - stream.tts_start_us;
        const int64_t pcm_dur_us = (int64_t) stream.tts_samples_out * 1000000LL / audio_service_get_sample_rate_hz();
        ESP_LOGI(TAG,
                 "播放汇总: %d帧 %u样本 = %dms@24k 实际耗时%dms %s",
                 stream.tts_frame_cnt,
                 (unsigned) stream.tts_samples_out,
                 (int) (pcm_dur_us / 1000),
                 (int) (play_elapsed_us / 1000),
                 play_elapsed_us > pcm_dur_us + 200000 ? "[疑似underrun/停顿]" : "");
    }
    ESP_LOGI(TAG,
             "voice HTTP 流式完成: status=%d, 上传=%u 接收=%u end=%d",
             result.status_code,
             (unsigned) result.uploaded_bytes,
             (unsigned) result.received_bytes,
             (int) stream.got_end);

    if (session_cancelled())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK || stream.got_error)
    {
        return err != ESP_OK ? err : ESP_FAIL;
    }
    return stream.got_end ? ESP_OK : ESP_FAIL;
}

static void run_voice_chat(void *arg)
{
    uint32_t              duration_ms    = (uint32_t) (uintptr_t) arg;
    esp_err_t             err            = ESP_FAIL;
    voice_service_event_t terminal_event = VOICE_SERVICE_EVENT_ERROR;
#if !CONFIG_DESKMATE_WAKE_WORD_ENABLE
    bool input_enabled = false;
#endif

    /* 分配录音缓冲：16kHz（AFE 输出）×时长×2字节，放 PSRAM */
    size_t   buf_samples = VOICE_SAMPLE_RATE * (duration_ms / 1000) + VOICE_SAMPLE_RATE;
    int16_t *record_buf  = NULL;
    record_buf           = (int16_t *) heap_caps_malloc(buf_samples * VOICE_SAMPLE_SIZE, MALLOC_CAP_SPIRAM);
    if (record_buf == NULL)
    {
        ESP_LOGE(TAG, "分配录音缓冲失败 (%u 样本)", (unsigned) buf_samples);
        goto cleanup;
    }

    /* 默认配置下仅在语音会话期间启用输入；必须先于 AFE 任务启动。 */
    err = audio_service_enable_input(true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "启用语音输入失败: %s", esp_err_to_name(err));
        goto cleanup;
    }
#if !CONFIG_DESKMATE_WAKE_WORD_ENABLE
    input_enabled = true;
#endif

    /* ---- 录音阶段：双麦 AFE 降噪 ---- */
    publish(VOICE_SERVICE_EVENT_RECORDING);
    size_t pcm_bytes = record_denoised_pcm(record_buf, buf_samples, duration_ms);
    if (pcm_bytes == 0)
    {
        ESP_LOGE(TAG, "录音失败");
        goto cleanup;
    }

    if (session_cancelled())
    {
        terminal_event = VOICE_SERVICE_EVENT_CANCELLED;
        goto cleanup;
    }

    /* ---- 流式上传 + 边收边播阶段 ---- */
    publish(VOICE_SERVICE_EVENT_THINKING);
    err = voice_ws_stream((const uint8_t *) record_buf, pcm_bytes);
    if (err != ESP_OK && err != ESP_ERR_INVALID_RESPONSE && !session_cancelled())
    {
        ESP_LOGW(TAG, "WebSocket 不可用(%s)，回退 HTTP 流式接口", esp_err_to_name(err));
        err = voice_http_stream((const uint8_t *) record_buf, pcm_bytes);
    }
    if (session_cancelled())
    {
        terminal_event = VOICE_SERVICE_EVENT_CANCELLED;
        goto cleanup;
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "语音对话失败");
        goto cleanup;
    }

    ESP_LOGI(TAG, "语音对话回合完成");
    terminal_event = VOICE_SERVICE_EVENT_DONE;

cleanup:
    free(record_buf);
#if !CONFIG_DESKMATE_WAKE_WORD_ENABLE
    if (input_enabled)
    {
        /* record_denoised_pcm 已完成 AFE stop/drain，之后才关闭 I2S 输入。 */
        (void) audio_service_enable_input(false);
    }
#endif
    finish_chat_task(terminal_event);
}

esp_err_t voice_service_init(void)
{
    if (s_ctx.state != VOICE_SERVICE_STATE_UNINITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(audio_service_is_initialized(), ESP_ERR_INVALID_STATE, TAG, "音频 Service 尚未初始化");
    ESP_RETURN_ON_FALSE(audio_processor_service_is_initialized(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "AFE 降噪 Service 尚未初始化");

    s_session_events = xEventGroupCreateStatic(&s_session_events_storage);
    ESP_RETURN_ON_FALSE(s_session_events != NULL, ESP_ERR_NO_MEM, TAG, "创建语音会话事件组失败");
    s_chat_stopped = xSemaphoreCreateBinary();
    if (s_chat_stopped == NULL)
    {
        vEventGroupDelete(s_session_events);
        s_session_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 播放 ring：PSRAM 缓冲 + 内部 StaticStreamBuffer 结构 */
    s_play_stream_buf = (uint8_t *) heap_caps_malloc(CONFIG_DESKMATE_VOICE_PLAY_STREAM_BYTES, MALLOC_CAP_SPIRAM);
    if (s_play_stream_buf == NULL)
    {
        vSemaphoreDelete(s_chat_stopped);
        s_chat_stopped = NULL;
        vEventGroupDelete(s_session_events);
        s_session_events = NULL;
        ESP_LOGE(TAG, "分配播放 ring 失败");
        return ESP_ERR_NO_MEM;
    }
    s_play_stream =
        xStreamBufferCreateStatic(CONFIG_DESKMATE_VOICE_PLAY_STREAM_BYTES, 1, s_play_stream_buf, &s_play_stream_struct);
    if (s_play_stream == NULL)
    {
        free(s_play_stream_buf);
        s_play_stream_buf = NULL;
        vSemaphoreDelete(s_chat_stopped);
        s_chat_stopped = NULL;
        vEventGroupDelete(s_session_events);
        s_session_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_session_lock);
    s_ctx.state      = VOICE_SERVICE_STATE_STOPPED;
    s_ctx.busy       = false;
    s_ctx.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_session_lock);
    ESP_LOGI(TAG,
             "语音服务初始化完成: 内部堆=%u PSRAM=%u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

esp_err_t voice_service_start(void)
{
    portENTER_CRITICAL(&s_session_lock);
    if (s_ctx.state == VOICE_SERVICE_STATE_RUNNING)
    {
        portEXIT_CRITICAL(&s_session_lock);
        return ESP_OK;
    }
    if (s_ctx.state != VOICE_SERVICE_STATE_STOPPED)
    {
        portEXIT_CRITICAL(&s_session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.state      = VOICE_SERVICE_STATE_RUNNING;
    s_ctx.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_session_lock);
    ESP_LOGI(TAG, "语音 Service 已启动，开放按键会话入口");
    return ESP_OK;
}

esp_err_t voice_service_stop(void)
{
    portENTER_CRITICAL(&s_session_lock);
    if (s_ctx.state == VOICE_SERVICE_STATE_STOPPED)
    {
        portEXIT_CRITICAL(&s_session_lock);
        return ESP_OK;
    }
    if (s_ctx.state != VOICE_SERVICE_STATE_RUNNING || s_ctx.busy)
    {
        portEXIT_CRITICAL(&s_session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.state = VOICE_SERVICE_STATE_STOPPING;
    portEXIT_CRITICAL(&s_session_lock);

    voice_service_task_status_t task_status = { 0 };
    voice_service_task_get_status(&task_status);
    if (task_status.chat_task_active || task_status.playback_task_active)
    {
        portENTER_CRITICAL(&s_session_lock);
        s_ctx.state = VOICE_SERVICE_STATE_RUNNING;
        portEXIT_CRITICAL(&s_session_lock);
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_session_lock);
    s_ctx.state      = VOICE_SERVICE_STATE_STOPPED;
    s_ctx.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_session_lock);
    ESP_LOGI(TAG, "语音 Service 已停止，新会话入口已关闭");
    return ESP_OK;
}

esp_err_t voice_service_deinit(void)
{
    portENTER_CRITICAL(&s_session_lock);
    if (s_ctx.state != VOICE_SERVICE_STATE_STOPPED || s_ctx.busy)
    {
        portEXIT_CRITICAL(&s_session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_session_lock);

    voice_service_task_status_t task_status = { 0 };
    voice_service_task_get_status(&task_status);
    if (task_status.chat_task_active || task_status.playback_task_active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    (void) xSemaphoreTake(s_chat_stopped, 0U);

    vStreamBufferDelete(s_play_stream);
    s_play_stream = NULL;
    free(s_play_stream_buf);
    s_play_stream_buf = NULL;
    vSemaphoreDelete(s_chat_stopped);
    s_chat_stopped = NULL;
    vEventGroupDelete(s_session_events);
    s_session_events = NULL;
    memset(&s_chat_config, 0, sizeof(s_chat_config));

    portENTER_CRITICAL(&s_session_lock);
    s_ctx = (voice_service_ctx_t) { 0 };
    portEXIT_CRITICAL(&s_session_lock);
    ESP_LOGI(TAG, "语音 Service 已反初始化");
    return ESP_OK;
}

esp_err_t voice_service_request_chat(const protocol_backend_context_t *backend, uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(protocol_backend_context_is_valid(backend), ESP_ERR_INVALID_ARG, TAG, "语音后端上下文无效");
    portENTER_CRITICAL(&s_session_lock);
    const bool available = s_ctx.state == VOICE_SERVICE_STATE_RUNNING;
    portEXIT_CRITICAL(&s_session_lock);
    if (!available)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms < 1000)
    {
        duration_ms = 1000;
    }
    if (duration_ms > 10000)
    {
        duration_ms = 10000;
    }
    /* 在进入 PSRAM 栈任务前把调用方上下文完整复制到内部 RAM。 */
    const voice_session_config_t session_config = {
        .backend = *backend,
    };

    if (!session_try_activate())
    {
        ESP_LOGW(TAG, "语音对话进行中，忽略新请求");
        return ESP_ERR_INVALID_STATE;
    }
    (void) xSemaphoreTake(s_chat_stopped, 0U);
    s_chat_config = session_config;
    xEventGroupClearBits(s_session_events, VOICE_SESSION_CANCELLED);
    const esp_err_t task_error = voice_service_task_start_chat(run_voice_chat, (void *) (uintptr_t) duration_ms);
    if (task_error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "创建 voice_chat 任务失败(需栈=%d): 内部堆=%u PSRAM=%u",
                 12288,
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        session_set_idle();
        (void) xSemaphoreGive(s_chat_stopped);
        return task_error;
    }
    return ESP_OK;
}

bool voice_service_is_busy(void)
{
    return session_is_busy();
}

esp_err_t voice_service_get_status_copy(voice_service_status_t *out_status)
{
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "语音状态输出为空");
    portENTER_CRITICAL(&s_session_lock);
    if (s_ctx.state == VOICE_SERVICE_STATE_UNINITIALIZED)
    {
        portEXIT_CRITICAL(&s_session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const voice_service_state_t state      = s_ctx.state;
    const bool                  busy       = s_ctx.busy;
    const esp_err_t             last_error = s_ctx.last_error;
    portEXIT_CRITICAL(&s_session_lock);

    voice_service_task_status_t task_status = { 0 };
    voice_service_task_get_status(&task_status);
    *out_status = (voice_service_status_t) {
        .state                = state,
        .session_busy         = busy,
        .chat_task_active     = task_status.chat_task_active,
        .playback_task_active = task_status.playback_task_active,
        .last_error           = last_error,
    };
    return ESP_OK;
}

static esp_err_t voice_ws_stream(const uint8_t *upload, size_t upload_len)
{
    char        url[256] = { 0 };
    const char *base     = s_chat_config.backend.base_url;
    const char *scheme   = strncmp(base, "https://", 8) == 0 ? "wss://" : "ws://";
    const char *host     = strstr(base, "://");
    const int   url_len  = snprintf(url,
                                    sizeof(url),
                                    "%s%s%sapi/v1/voice/ws",
                                    scheme,
                                    host != NULL ? host + 3 : base,
                                    base[strlen(base) - 1] == '/' ? "" : "/");
    ESP_RETURN_ON_FALSE(url_len > 0 && url_len < (int) sizeof(url), ESP_ERR_INVALID_SIZE, TAG, "WebSocket URL 过长");

    char headers[192] = { 0 };
    ESP_RETURN_ON_ERROR(protocol_identity_format_websocket_headers(headers,
                                                                   sizeof(headers),
                                                                   s_chat_config.backend.token,
                                                                   s_chat_config.backend.device_id),
                        TAG,
                        "WebSocket 身份头过长");

    const transport_websocket_config_t config = {
        .url                  = url,
        .headers              = headers,
        .network_timeout_ms   = CONFIG_DESKMATE_VOICE_HTTP_TIMEOUT_MS,
        .connect_timeout_ms   = CONFIG_DESKMATE_VOICE_WS_CONNECT_TIMEOUT_MS,
        .receive_buffer_bytes = CONFIG_DESKMATE_VOICE_WS_BUFFER_SIZE,
        .max_message_bytes    = VOICE_PROTOCOL_MAX_PAYLOAD + VOICE_PROTOCOL_HEADER_SIZE,
        .receive_queue_length = CONFIG_DESKMATE_VOICE_WS_RX_QUEUE_LEN,
    };
    transport_websocket_t *socket = NULL;
    ESP_RETURN_ON_ERROR(transport_websocket_open(&config, &socket), TAG, "连接 Voice WebSocket 失败");

    voice_protocol_decoder_t *decoder = voice_protocol_decoder_create();
    if (decoder == NULL)
    {
        transport_websocket_close(socket);
        return ESP_ERR_NO_MEM;
    }
    stream_ctx_t stream = { 0 };
    esp_err_t    err    = playback_start(&stream);
    if (err != ESP_OK)
    {
        voice_protocol_decoder_destroy(decoder);
        transport_websocket_close(socket);
        return err;
    }

    err = transport_websocket_send_text(socket, voice_protocol_start_message(), 3000);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "发送 WebSocket START 失败: %s", esp_err_to_name(err));
    }
    size_t uploaded      = 0;
    int    upload_chunks = 0;
    while (err == ESP_OK && uploaded < upload_len && !session_cancelled())
    {
        size_t chunk_len = upload_len - uploaded;
        if (chunk_len > CONFIG_DESKMATE_VOICE_WS_UPLOAD_CHUNK_BYTES)
        {
            chunk_len = CONFIG_DESKMATE_VOICE_WS_UPLOAD_CHUNK_BYTES;
        }
        err = transport_websocket_send_binary(socket, upload + uploaded, chunk_len, 3000);
        if (err == ESP_OK)
        {
            uploaded += chunk_len;
            upload_chunks++;
        }
        else
        {
            ESP_LOGW(TAG,
                     "发送 WebSocket PCM 失败: offset=%u chunk=%u err=%s",
                     (unsigned) uploaded,
                     (unsigned) chunk_len,
                     esp_err_to_name(err));
        }
    }
    if (session_cancelled())
    {
        (void) transport_websocket_send_text(socket, voice_protocol_cancel_message(), 500);
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK)
    {
        err = transport_websocket_send_text(socket, voice_protocol_end_input_message(), 3000);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "发送 WebSocket END_INPUT 失败: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WebSocket 上行完成: bytes=%u chunks=%d，等待 ASR/TTS 响应", (unsigned) uploaded, upload_chunks);
    }

    uint32_t last_activity_ms = (uint32_t) (esp_timer_get_time() / 1000);
    while (err == ESP_OK && !stream.got_end && !stream.got_error && !session_cancelled())
    {
        transport_websocket_message_t message     = { 0 };
        const esp_err_t               receive_err = transport_websocket_receive(socket, &message, 20);
        if (receive_err == ESP_OK)
        {
            if (message.binary)
            {
                err = voice_protocol_decoder_feed(decoder, message.data, message.len, on_stream_frame, &stream);
            }
            transport_websocket_message_release(&message);
            last_activity_ms = (uint32_t) (esp_timer_get_time() / 1000);
        }
        else if (receive_err != ESP_ERR_TIMEOUT)
        {
            err = receive_err;
            break;
        }

        const uint32_t now_ms = (uint32_t) (esp_timer_get_time() / 1000);
        if ((uint32_t) (now_ms - last_activity_ms) >= CONFIG_DESKMATE_VOICE_WS_IDLE_TIMEOUT_MS)
        {
            ESP_LOGE(TAG, "WebSocket 空闲超时: %u ms", (unsigned) CONFIG_DESKMATE_VOICE_WS_IDLE_TIMEOUT_MS);
            err = ESP_ERR_TIMEOUT;
        }
    }
    if (session_cancelled())
    {
        (void) transport_websocket_send_text(socket, voice_protocol_cancel_message(), 500);
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK && (!stream.got_end || stream.got_error))
    {
        err = stream.received_frame ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL;
    }

    (void) playback_finish(&stream, session_cancelled() || stream.got_error || !stream.got_end);
    const uint32_t dropped = transport_websocket_dropped_messages(socket);
    transport_websocket_close(socket);
    voice_protocol_decoder_destroy(decoder);
    if (dropped > 0)
    {
        ESP_LOGW(TAG, "WebSocket 下行共丢弃消息: %u", (unsigned) dropped);
    }
    return err;
}

esp_err_t voice_service_cancel(void)
{
    portENTER_CRITICAL(&s_session_lock);
    const bool initialized = s_ctx.state != VOICE_SERVICE_STATE_UNINITIALIZED;
    portEXIT_CRITICAL(&s_session_lock);
    if (!initialized || s_session_events == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!session_is_busy())
    {
        return ESP_OK;
    }
    xEventGroupSetBits(s_session_events, VOICE_SESSION_CANCELLED);
    ESP_LOGI(TAG, "已请求取消语音会话");
    return ESP_OK;
}
