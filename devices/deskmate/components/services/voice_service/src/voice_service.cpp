/*
 * 文件职责：封装录音、网络会话与 TTS PCM 提交的语音交互闭环。
 * 主要依赖：audio_processor_service、audio_service、transport、voice_protocol。
 * 调用方：App 业务流程（按键或唤醒词触发）。
 *
 * 数据流：
 *   录音：(MIC) → ES7210 2通道 → audio_processor_service (双麦 AFE 降噪)
 *         → 单声道 16kHz PCM → WebSocket 上传（失败时回退 HTTP）
 *   播放：server 流式帧响应 → 逐帧解析 → Audio Service 唯一 PCM 输出事务
 */
#include "voice_service.h"
#include "voice_service_internal.hpp"

#include <assert.h>
#include <new>
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
#include "freertos/task.h"
#include "protocol_identity.h"
#include "protocol_url.h"
#include "voice_protocol.h"

static const char *TAG = "voice_service";

#define VOICE_SAMPLE_RATE            16000 /* AFE 降噪后输出采样率 */
#define VOICE_TTS_SAMPLE_RATE        24000
/* 单个 int16 样本 */
#define VOICE_SAMPLE_SIZE            2
#define VOICE_PCM_WRITE_TIMEOUT_MS   2000U
#define VOICE_PCM_DISCARD_TIMEOUT_MS 3000U

/* HTTP/VAD/WebSocket 超时与 Audio Service PCM 缓冲见 Kconfig: DeskMate Audio/Voice。 */

/* HTTP 流式读取缓冲 */
#define VOICE_HTTP_READ_BUF          2048

ESP_EVENT_DEFINE_BASE(VOICE_SERVICE_EVENT);

static VoiceServiceRuntime *s_runtime;

#define VOICE_SESSION_CANCELLED BIT0

/** @brief 单轮响应解析与 Audio Service PCM 流状态。 */
struct stream_ctx_t
{
    bool     speaking_started{};
    bool     got_end{};
    bool     got_error{};
    bool     received_frame{};
    uint64_t pcm_stream_id{};
    /* ── 播放统计（用于会话结束汇总日志）── */
    int64_t tts_start_us{};    /* 首帧 TTS 到达时刻 */
    size_t  tts_samples_out{}; /* 累计提交的 24kHz 样本数 */
    int     tts_frame_cnt{};   /* TTS 帧计数 */
};

/** @brief WebSocket 尝试事实，用于禁止部分上传后的 HTTP 重复提交。 */
struct voice_ws_attempt_t
{
    size_t uploaded_pcm_bytes{};
    bool   received_response{};
};

static esp_err_t voice_ws_stream(const uint8_t *upload, size_t upload_len, voice_ws_attempt_t *out_attempt);

VoiceServiceRuntime::~VoiceServiceRuntime() noexcept
{
    assert(session_events == nullptr);
    assert(chat_task == nullptr);
    assert(record_buffer == nullptr);
}

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
    VoiceServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr)
    {
        return false;
    }
    bool activated = false;
    portENTER_CRITICAL(&runtime->session_lock);
    if (runtime->state == VOICE_SERVICE_STATE_RUNNING && !runtime->busy)
    {
        runtime->busy = true;
        activated     = true;
    }
    portEXIT_CRITICAL(&runtime->session_lock);
    return activated;
}

static void session_set_idle(void)
{
    VoiceServiceRuntime *runtime = s_runtime;
    portENTER_CRITICAL(&runtime->session_lock);
    runtime->busy = false;
    portEXIT_CRITICAL(&runtime->session_lock);
}

static bool session_is_busy(void)
{
    VoiceServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr)
    {
        return false;
    }
    portENTER_CRITICAL(&runtime->session_lock);
    const bool busy = runtime->busy;
    portEXIT_CRITICAL(&runtime->session_lock);
    return busy;
}

/* ── 流式帧回调 ──────────────────────────────────────── */

static bool session_cancelled(void)
{
    VoiceServiceRuntime *runtime = s_runtime;
    return runtime != nullptr && runtime->session_events != nullptr
           && (xEventGroupGetBits(runtime->session_events) & VOICE_SESSION_CANCELLED) != 0U;
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
}

/** @brief 首个有效 TTS PCM 帧到达时打开 24 kHz 单声道流。 */
static esp_err_t pcm_stream_open(stream_ctx_t *ctx)
{
    if (ctx->pcm_stream_id != 0U)
    {
        return ESP_OK;
    }
    const audio_service_pcm_stream_config_t config{
        .sample_rate_hz = VOICE_TTS_SAMPLE_RATE,
        .channel_count  = 1U,
    };
    const esp_err_t error = audio_service_open_pcm_stream(&config, VOICE_PCM_DISCARD_TIMEOUT_MS, &ctx->pcm_stream_id);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "打开 TTS PCM 流失败: %s", esp_err_to_name(error));
    }
    return error;
}

/** @brief 正常路径排空 PCM，错误或取消路径丢弃；超时后追加一次丢弃收敛请求。 */
static esp_err_t pcm_stream_finish(stream_ctx_t *ctx, bool discard)
{
    if (ctx->pcm_stream_id == 0U)
    {
        return ESP_OK;
    }
    const uint32_t drain_timeout_ms =
        static_cast<uint32_t>((static_cast<uint64_t>(CONFIG_DESKMATE_AUDIO_PCM_STREAM_BYTES) * 1000ULL)
                              / (VOICE_TTS_SAMPLE_RATE * VOICE_SAMPLE_SIZE))
        + 2000U;
    const uint32_t timeout_ms = discard ? VOICE_PCM_DISCARD_TIMEOUT_MS : drain_timeout_ms;
    esp_err_t      error      = audio_service_close_pcm_stream(ctx->pcm_stream_id, discard, timeout_ms);
    if (error == ESP_ERR_TIMEOUT && !discard)
    {
        ESP_LOGE(TAG, "等待 TTS PCM 排空超时，改为丢弃收敛");
        error = audio_service_close_pcm_stream(ctx->pcm_stream_id, true, VOICE_PCM_DISCARD_TIMEOUT_MS);
    }
    if (error != ESP_OK)
    {
        ctx->got_error = true;
        ESP_LOGE(TAG,
                 "关闭 TTS PCM 流失败: stream=%llu error=%s",
                 static_cast<unsigned long long>(ctx->pcm_stream_id),
                 esp_err_to_name(error));
    }
    ctx->pcm_stream_id = 0U;
    return error;
}

static void on_stream_frame(voice_protocol_frame_type_t type, const uint8_t *payload, size_t len, void *arg)
{
    stream_ctx_t *ctx = (stream_ctx_t *) arg;
    if (ctx->got_error || ctx->got_end || session_cancelled())
    {
        return;
    }
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
            if (pcm_stream_open(ctx) != ESP_OK)
            {
                ctx->got_error = true;
                break;
            }
            /* 首个 PCM 分片成功取得唯一输出事务后切换到播放状态。 */
            if (!ctx->speaking_started)
            {
                ctx->speaking_started = true;
                ctx->tts_start_us     = esp_timer_get_time();
                ctx->tts_samples_out  = 0;
                ctx->tts_frame_cnt    = 0;
                publish(VOICE_SERVICE_EVENT_SPEAKING);
                ESP_LOGI(TAG, "开始流式播放");
            }

            /* Hub 契约固定为 24 kHz 单声道 16-bit；Audio Service 负责复制、抗抖动和输出重采样。 */
            const size_t    sample_count = len / VOICE_SAMPLE_SIZE;
            size_t          written      = 0U;
            const esp_err_t write_error =
                audio_service_write_pcm_stream_borrow(ctx->pcm_stream_id,
                                                      reinterpret_cast<const int16_t *>(payload),
                                                      sample_count,
                                                      VOICE_PCM_WRITE_TIMEOUT_MS,
                                                      &written);
            ctx->tts_samples_out += written;
            if (write_error != ESP_OK || written != sample_count)
            {
                ESP_LOGE(TAG,
                         "提交 TTS PCM 失败: expected=%u actual=%u error=%s",
                         (unsigned) sample_count,
                         (unsigned) written,
                         esp_err_to_name(write_error));
                ctx->got_error = true;
                break;
            }

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
    VoiceServiceRuntime *runtime  = s_runtime;
    char                 url[256] = { 0 };
    ESP_RETURN_ON_ERROR(protocol_url_build(url, sizeof(url), runtime->chat_backend.base_url, "api/v1/voice/chat"),
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
                                  runtime->chat_backend.token,
                                  runtime->chat_backend.device_id,
                                  auth_header,
                                  sizeof(auth_header));

    voice_protocol_decoder_t *decoder = voice_protocol_decoder_create();
    ESP_RETURN_ON_FALSE(decoder != nullptr, ESP_ERR_NO_MEM, TAG, "创建 Voice 帧解码器失败");
    stream_ctx_t stream{};

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
    transport_http_stream_result_t result{};
    esp_err_t                      err = transport_http_stream_borrow(&request, &result);

    const bool      discard            = session_cancelled() || stream.got_error || !stream.got_end || err != ESP_OK;
    const esp_err_t close_error        = pcm_stream_finish(&stream, discard);
    if (err == ESP_OK && close_error != ESP_OK)
    {
        err = close_error;
    }
    voice_protocol_decoder_destroy(decoder);

    if (stream.speaking_started)
    {
        const int64_t play_elapsed_us = esp_timer_get_time() - stream.tts_start_us;
        const int64_t pcm_dur_us = static_cast<int64_t>(stream.tts_samples_out) * 1000000LL / VOICE_TTS_SAMPLE_RATE;
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

void voice_service_run_chat(VoiceServiceRuntime *runtime)
{
    const uint32_t        duration_ms    = runtime->chat_duration_ms;
    esp_err_t             err            = ESP_FAIL;
    voice_service_event_t terminal_event = VOICE_SERVICE_EVENT_ERROR;
    const size_t          buffer_samples = VOICE_SAMPLE_RATE * (duration_ms / 1000U) + VOICE_SAMPLE_RATE;
    size_t                pcm_bytes      = 0U;
    voice_ws_attempt_t    websocket_attempt{};
    bool                  can_fallback_http = false;

    /* 分配录音缓冲：16kHz（AFE 输出）×时长×2字节，放 PSRAM */
    runtime->record_buffer                  = static_cast<int16_t *>(
        heap_caps_malloc(buffer_samples * VOICE_SAMPLE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (runtime->record_buffer == nullptr)
    {
        ESP_LOGE(TAG, "分配录音缓冲失败 (%u 样本)", (unsigned) buffer_samples);
        goto cleanup;
    }
    runtime->record_capacity_samples = buffer_samples;

    /* ---- 录音阶段：双麦 AFE 降噪 ---- */
    publish(VOICE_SERVICE_EVENT_RECORDING);
    pcm_bytes = record_denoised_pcm(runtime->record_buffer, buffer_samples, duration_ms);
    if (session_cancelled())
    {
        terminal_event = VOICE_SERVICE_EVENT_CANCELLED;
        goto cleanup;
    }
    if (pcm_bytes == 0U)
    {
        ESP_LOGE(TAG, "录音失败");
        goto cleanup;
    }

    /* ---- 流式上传 + 边收边播阶段 ---- */
    publish(VOICE_SERVICE_EVENT_THINKING);
    err = voice_ws_stream(reinterpret_cast<const uint8_t *>(runtime->record_buffer), pcm_bytes, &websocket_attempt);
    can_fallback_http = err != ESP_OK && !session_cancelled() && websocket_attempt.uploaded_pcm_bytes == 0U
                        && !websocket_attempt.received_response;
    if (can_fallback_http)
    {
        ESP_LOGW(TAG, "WebSocket 不可用(%s)，回退 HTTP 流式接口", esp_err_to_name(err));
        err = voice_http_stream(reinterpret_cast<const uint8_t *>(runtime->record_buffer), pcm_bytes);
    }
    else if (err != ESP_OK && !session_cancelled())
    {
        ESP_LOGW(TAG,
                 "WebSocket 已提交 PCM 或收到响应，禁止 HTTP 重复提交: uploaded=%u response=%d error=%s",
                 (unsigned) websocket_attempt.uploaded_pcm_bytes,
                 (int) websocket_attempt.received_response,
                 esp_err_to_name(err));
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
    heap_caps_free(runtime->record_buffer);
    runtime->record_buffer           = nullptr;
    runtime->record_capacity_samples = 0U;
    portENTER_CRITICAL(&runtime->session_lock);
    runtime->last_error = terminal_event == VOICE_SERVICE_EVENT_ERROR ? (err != ESP_OK ? err : ESP_FAIL) : ESP_OK;
    portEXIT_CRITICAL(&runtime->session_lock);
    finish_chat_task(terminal_event);
}

esp_err_t voice_service_init(void)
{
    if (s_runtime != nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    audio_service_status_t audio_status{};
    ESP_RETURN_ON_ERROR(audio_service_get_status_copy(&audio_status), TAG, "Audio Service 尚未初始化");
    audio_processor_service_status_t processor_status{};
    ESP_RETURN_ON_ERROR(audio_processor_service_get_status_copy(&processor_status), TAG, "AFE 降噪 Service 尚未初始化");

    void *storage = heap_caps_calloc(1U, sizeof(VoiceServiceRuntime), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(storage != nullptr, ESP_ERR_NO_MEM, TAG, "创建 Voice Service Runtime 失败");
    auto *runtime           = new (storage) VoiceServiceRuntime();
    s_runtime               = runtime;

    runtime->session_events = xEventGroupCreateStatic(&runtime->session_events_storage);
    if (runtime->session_events == nullptr)
    {
        s_runtime = nullptr;
        runtime->~VoiceServiceRuntime();
        heap_caps_free(runtime);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&runtime->session_lock);
    runtime->state      = VOICE_SERVICE_STATE_STOPPED;
    runtime->busy       = false;
    runtime->last_error = ESP_OK;
    portEXIT_CRITICAL(&runtime->session_lock);
    ESP_LOGI(TAG,
             "语音服务初始化完成: 内部堆=%u PSRAM=%u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

esp_err_t voice_service_start(void)
{
    VoiceServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&runtime->session_lock);
    if (runtime->state == VOICE_SERVICE_STATE_RUNNING)
    {
        portEXIT_CRITICAL(&runtime->session_lock);
        return ESP_OK;
    }
    if (runtime->state != VOICE_SERVICE_STATE_STOPPED)
    {
        portEXIT_CRITICAL(&runtime->session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    runtime->state      = VOICE_SERVICE_STATE_RUNNING;
    runtime->last_error = ESP_OK;
    portEXIT_CRITICAL(&runtime->session_lock);
    ESP_LOGI(TAG, "语音 Service 已启动，开放按键会话入口");
    return ESP_OK;
}

esp_err_t voice_service_stop(void)
{
    VoiceServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&runtime->session_lock);
    if (runtime->state == VOICE_SERVICE_STATE_STOPPED)
    {
        portEXIT_CRITICAL(&runtime->session_lock);
        return ESP_OK;
    }
    if (runtime->state != VOICE_SERVICE_STATE_RUNNING || runtime->busy)
    {
        portEXIT_CRITICAL(&runtime->session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    runtime->state = VOICE_SERVICE_STATE_STOPPING;
    portEXIT_CRITICAL(&runtime->session_lock);

    if (voice_service_chat_task_active(runtime))
    {
        portENTER_CRITICAL(&runtime->session_lock);
        runtime->state = VOICE_SERVICE_STATE_RUNNING;
        portEXIT_CRITICAL(&runtime->session_lock);
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&runtime->session_lock);
    runtime->state      = VOICE_SERVICE_STATE_STOPPED;
    runtime->last_error = ESP_OK;
    portEXIT_CRITICAL(&runtime->session_lock);
    ESP_LOGI(TAG, "语音 Service 已停止，新会话入口已关闭");
    return ESP_OK;
}

esp_err_t voice_service_deinit(void)
{
    VoiceServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&runtime->session_lock);
    if (runtime->state != VOICE_SERVICE_STATE_STOPPED || runtime->busy)
    {
        portEXIT_CRITICAL(&runtime->session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&runtime->session_lock);

    if (voice_service_chat_task_active(runtime))
    {
        return ESP_ERR_INVALID_STATE;
    }
    vEventGroupDelete(runtime->session_events);
    runtime->session_events = nullptr;
    memset(&runtime->chat_backend, 0, sizeof(runtime->chat_backend));

    s_runtime = nullptr;
    runtime->~VoiceServiceRuntime();
    heap_caps_free(runtime);
    ESP_LOGI(TAG, "语音 Service 已反初始化");
    return ESP_OK;
}

esp_err_t voice_service_request_chat(const protocol_backend_context_t *backend, uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(protocol_backend_context_is_valid(backend), ESP_ERR_INVALID_ARG, TAG, "语音后端上下文无效");
    VoiceServiceRuntime *runtime = s_runtime;
    ESP_RETURN_ON_FALSE(runtime != nullptr, ESP_ERR_INVALID_STATE, TAG, "Voice Service 尚未初始化");
    portENTER_CRITICAL(&runtime->session_lock);
    const bool available = runtime->state == VOICE_SERVICE_STATE_RUNNING;
    portEXIT_CRITICAL(&runtime->session_lock);
    if (!available)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms < 2000U)
    {
        duration_ms = 2000U;
    }
    if (duration_ms > 10000)
    {
        duration_ms = 10000;
    }
    if (!session_try_activate())
    {
        ESP_LOGW(TAG, "语音对话进行中，忽略新请求");
        return ESP_ERR_INVALID_STATE;
    }
    runtime->chat_backend     = *backend;
    runtime->chat_duration_ms = duration_ms;
    xEventGroupClearBits(runtime->session_events, VOICE_SESSION_CANCELLED);
    const esp_err_t task_error = voice_service_chat_task_start(runtime);
    if (task_error != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "创建 voice_chat 任务失败(需栈=%d): 内部堆=%u PSRAM=%u",
                 12288,
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        portENTER_CRITICAL(&runtime->session_lock);
        runtime->last_error = task_error;
        portEXIT_CRITICAL(&runtime->session_lock);
        session_set_idle();
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
    ESP_RETURN_ON_FALSE(out_status != nullptr, ESP_ERR_INVALID_ARG, TAG, "语音状态输出为空");
    VoiceServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&runtime->session_lock);
    const voice_service_state_t state      = runtime->state;
    const bool                  busy       = runtime->busy;
    const esp_err_t             last_error = runtime->last_error;
    portEXIT_CRITICAL(&runtime->session_lock);

    *out_status = {
        .state            = state,
        .session_busy     = busy,
        .chat_task_active = voice_service_chat_task_active(runtime),
        .last_error       = last_error,
    };
    return ESP_OK;
}

static esp_err_t voice_ws_stream(const uint8_t *upload, size_t upload_len, voice_ws_attempt_t *out_attempt)
{
    VoiceServiceRuntime *runtime = s_runtime;
    *out_attempt                 = {};
    char        url[256]         = { 0 };
    const char *base             = runtime->chat_backend.base_url;
    const char *scheme           = strncmp(base, "https://", 8) == 0 ? "wss://" : "ws://";
    const char *host             = strstr(base, "://");
    const int   url_len          = snprintf(url,
                                            sizeof(url),
                                            "%s%s%sapi/v1/voice/ws",
                                            scheme,
                                            host != nullptr ? host + 3 : base,
                                            base[strlen(base) - 1] == '/' ? "" : "/");
    ESP_RETURN_ON_FALSE(url_len > 0 && url_len < (int) sizeof(url), ESP_ERR_INVALID_SIZE, TAG, "WebSocket URL 过长");

    char headers[192] = { 0 };
    ESP_RETURN_ON_ERROR(protocol_identity_format_websocket_headers(headers,
                                                                   sizeof(headers),
                                                                   runtime->chat_backend.token,
                                                                   runtime->chat_backend.device_id),
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
    transport_websocket_t *socket = nullptr;
    ESP_RETURN_ON_ERROR(transport_websocket_open(&config, &socket), TAG, "连接 Voice WebSocket 失败");

    voice_protocol_decoder_t *decoder = voice_protocol_decoder_create();
    if (decoder == nullptr)
    {
        transport_websocket_close(socket);
        return ESP_ERR_NO_MEM;
    }
    stream_ctx_t stream{};

    esp_err_t err = transport_websocket_send_text(socket, voice_protocol_start_message(), 3000);
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
            out_attempt->uploaded_pcm_bytes = uploaded;
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
        transport_websocket_message_t message{};
        const esp_err_t               receive_err = transport_websocket_receive(socket, &message, 20);
        if (receive_err == ESP_OK)
        {
            out_attempt->received_response = true;
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

    const bool      discard     = session_cancelled() || stream.got_error || !stream.got_end || err != ESP_OK;
    const esp_err_t close_error = pcm_stream_finish(&stream, discard);
    if (err == ESP_OK && close_error != ESP_OK)
    {
        err = close_error;
    }
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
    VoiceServiceRuntime *runtime = s_runtime;
    if (runtime == nullptr || runtime->session_events == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!session_is_busy())
    {
        return ESP_OK;
    }
    xEventGroupSetBits(runtime->session_events, VOICE_SESSION_CANCELLED);
    ESP_LOGI(TAG, "已请求取消语音会话");
    return ESP_OK;
}
