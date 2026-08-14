#include "remote_log_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** @brief 把 ESP-IDF 日志级别转换为协议使用的单字符级别 */
static char remote_log_level_letter(esp_log_level_t level)
{
    switch (level)
    {
        case ESP_LOG_ERROR:
            return 'E';
        case ESP_LOG_WARN:
            return 'W';
        case ESP_LOG_DEBUG:
            return 'D';
        case ESP_LOG_VERBOSE:
            return 'V';
        case ESP_LOG_INFO:
        default:
            return 'I';
    }
}

/**
 * @brief 判断一条 Log V2 日志是否会通过当前公开级别配置
 *
 * @param[in] config ESP-IDF Log V2 日志配置
 * @param[in] tag 日志标签，可为空
 * @return true 当前级别允许输出；false 日志被主级别、默认级别或 Tag 级别过滤
 */
static bool remote_log_level_enabled(esp_log_config_t config, const char *tag)
{
    const esp_log_level_t level = config.opts.log_level;
    if (level == ESP_LOG_NONE)
    {
        return false;
    }
#if CONFIG_LOG_MASTER_LEVEL
    if (esp_log_get_level_master() < level)
    {
        return false;
    }
#endif
    if (config.opts.constrained_env)
    {
        return esp_log_get_default_level() >= level;
    }
    return esp_log_level_get(tag) >= level;
}

/** @brief 返回 UTF-8 首字节声明的字符长度；非法首字节返回 0 */
static size_t remote_log_utf8_sequence_length(unsigned char lead)
{
    if ((lead & 0x80U) == 0U)
    {
        return 1U;
    }
    if (lead >= 0xC2U && lead <= 0xDFU)
    {
        return 2U;
    }
    if (lead >= 0xE0U && lead <= 0xEFU)
    {
        return 3U;
    }
    if (lead >= 0xF0U && lead <= 0xF4U)
    {
        return 4U;
    }
    return 0U;
}

/** @brief 移除固定缓冲区截断产生的不完整 UTF-8 尾部 */
static void remote_log_trim_incomplete_utf8(char *text)
{
    const size_t length       = strlen(text);
    size_t       valid_length = 0U;

    while (valid_length < length)
    {
        const size_t sequence_length = remote_log_utf8_sequence_length((unsigned char) text[valid_length]);
        if (sequence_length == 0U || valid_length + sequence_length > length)
        {
            break;
        }

        bool sequence_complete = true;
        for (size_t index = 1U; index < sequence_length; ++index)
        {
            const unsigned char byte = (unsigned char) text[valid_length + index];
            if ((byte & 0xC0U) != 0x80U)
            {
                sequence_complete = false;
                break;
            }
        }
        if (!sequence_complete)
        {
            break;
        }
        valid_length += sequence_length;
    }

    if (valid_length < length)
    {
        text[valid_length] = '\0';
    }
}

static void remote_log_copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0U)
    {
        return;
    }

    size_t copy_length = 0U;
    while (copy_length + 1U < capacity && source[copy_length] != '\0')
    {
        ++copy_length;
    }
    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
    remote_log_trim_incomplete_utf8(destination);
}

static void remote_log_record_drop(void)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    ++g_remote_log_runtime.dropped_lines;
    taskEXIT_CRITICAL(&g_remote_log_lock);
}

static void remote_log_capture_release(void)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    if (g_remote_log_runtime.active_captures > 0U)
    {
        --g_remote_log_runtime.active_captures;
    }
    taskEXIT_CRITICAL(&g_remote_log_lock);
}

/**
 * @brief 捕获一条 Log V2 结构化日志并非阻塞投递
 *
 * 上传 Task 自己产生的日志不入队，避免协议与 HTTP 日志形成反馈循环。
 *
 * @param[in] config ESP-IDF Log V2 日志配置
 * @param[in] tag 日志标签，可为空
 * @param[in] format 正文格式串
 * @param[in] args printf 参数
 */
static void remote_log_capture(esp_log_config_t config, const char *tag, const char *format, va_list args)
{
    taskENTER_CRITICAL(&g_remote_log_lock);
    const bool         capture_enabled = g_remote_log_runtime.initialized && g_remote_log_runtime.capture_enabled;
    const TaskHandle_t uploader_task   = g_remote_log_runtime.task;
    QueueHandle_t      events          = g_remote_log_runtime.events;
    SemaphoreHandle_t  log_slots       = g_remote_log_runtime.log_slots;
    if (capture_enabled)
    {
        ++g_remote_log_runtime.active_captures;
    }
    taskEXIT_CRITICAL(&g_remote_log_lock);

    if (!capture_enabled || events == NULL || log_slots == NULL || xTaskGetCurrentTaskHandle() == uploader_task)
    {
        if (capture_enabled)
        {
            remote_log_capture_release();
        }
        return;
    }

    remote_log_event_t event = {
        .type = REMOTE_LOG_EVENT_LINE,
    };
    event.line.level[0] = remote_log_level_letter(config.opts.log_level);
    event.line.level[1] = '\0';
    remote_log_copy_text(event.line.tag, sizeof(event.line.tag), tag != NULL ? tag : "esp32");
    vsnprintf(event.line.raw, sizeof(event.line.raw), format != NULL ? format : "", args);
    event.line.raw[strcspn(event.line.raw, "\r\n")] = '\0';
    remote_log_trim_incomplete_utf8(event.line.raw);
    remote_log_copy_text(event.line.message, sizeof(event.line.message), event.line.raw);

    taskENTER_CRITICAL(&g_remote_log_lock);
    event.line.seq = g_remote_log_runtime.next_sequence++;
    taskEXIT_CRITICAL(&g_remote_log_lock);
    event.line.uptime_ms = esp_log_timestamp();

    if (xSemaphoreTake(log_slots, 0U) != pdTRUE)
    {
        remote_log_record_drop();
        remote_log_capture_release();
        return;
    }

    taskENTER_CRITICAL(&g_remote_log_lock);
    ++g_remote_log_runtime.captured_lines;
    ++g_remote_log_runtime.queued_lines;
    taskEXIT_CRITICAL(&g_remote_log_lock);
    if (xQueueSend(events, &event, 0U) != pdTRUE)
    {
        (void) xSemaphoreGive(log_slots);
        taskENTER_CRITICAL(&g_remote_log_lock);
        --g_remote_log_runtime.captured_lines;
        --g_remote_log_runtime.queued_lines;
        taskEXIT_CRITICAL(&g_remote_log_lock);
        remote_log_record_drop();
        remote_log_capture_release();
        return;
    }
    remote_log_capture_release();
}

void __wrap_esp_log(esp_log_config_t config, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);

    va_list output_args;
    va_copy(output_args, args);
    esp_log_va(config, tag, format, output_args);
    va_end(output_args);

    if (remote_log_level_enabled(config, tag))
    {
        remote_log_capture(config, tag, format, args);
    }
    va_end(args);
}
