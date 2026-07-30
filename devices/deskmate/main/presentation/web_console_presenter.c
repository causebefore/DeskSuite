/*
 * 文件职责：把 Application 推送的纯展示事实转换为线程安全的设备端 View Model。
 */
#include "web_console_presenter.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE          s_view_lock = portMUX_INITIALIZER_UNLOCKED;
static web_console_view_model_t s_view;
static bool                  s_initialized;
static uint64_t              s_applied_revision;

/** @brief 返回 Presenter 状态对应的固定中文标题 */
static const char *title_for_state(web_console_presenter_state_t state)
{
    switch (state)
    {
        case WEB_CONSOLE_PRESENTER_STATE_STOPPED:
            return "网页控制台";
        case WEB_CONSOLE_PRESENTER_STATE_CHECKING_STORAGE:
            return "正在检查 SD 卡";
        case WEB_CONSOLE_PRESENTER_STATE_ACQUIRING_NETWORK:
            return "正在申请网络";
        case WEB_CONSOLE_PRESENTER_STATE_STARTING_SERVICE:
            return "正在启动服务";
        case WEB_CONSOLE_PRESENTER_STATE_RUNNING:
            return "网页控制台已开启";
        case WEB_CONSOLE_PRESENTER_STATE_STOPPING:
            return "正在关闭服务";
        case WEB_CONSOLE_PRESENTER_STATE_ERROR:
        default:
            return "启动或关闭失败";
    }
}

/**
 * @brief 把字节容量格式化为 B/KiB/MiB/GiB，非整单位值保留一位小数
 *
 * @param[in] bytes 原始字节数
 * @param[out] output 调用方固定输出缓冲区
 * @param[in] output_size 输出缓冲区容量
 */
static void format_capacity(uint64_t bytes, char *output, size_t output_size)
{
    static const uint64_t kib = 1024ULL;
    static const uint64_t mib = 1024ULL * 1024ULL;
    static const uint64_t gib = 1024ULL * 1024ULL * 1024ULL;

    const char *unit          = "B";
    uint64_t    unit_bytes    = 1ULL;
    if (bytes >= gib)
    {
        unit       = "GiB";
        unit_bytes = gib;
    }
    else if (bytes >= mib)
    {
        unit       = "MiB";
        unit_bytes = mib;
    }
    else if (bytes >= kib)
    {
        unit       = "KiB";
        unit_bytes = kib;
    }

    if (unit_bytes == 1ULL)
    {
        (void) snprintf(output, output_size, "%llu B", (unsigned long long) bytes);
    }
    else if ((bytes % unit_bytes) == 0ULL)
    {
        (void) snprintf(output, output_size, "%llu %s", (unsigned long long) (bytes / unit_bytes), unit);
    }
    else
    {
        (void) snprintf(output, output_size, "%.1f %s", (double) bytes / (double) unit_bytes, unit);
    }
}

/** @brief 构造停止态初始 View Model */
static web_console_view_model_t make_stopped_view(void)
{
    web_console_view_model_t view = {
        .state        = WEB_CONSOLE_PRESENTER_STATE_STOPPED,
        .exit_allowed = true,
        .error        = ESP_OK,
    };
    (void) snprintf(view.title, sizeof(view.title), "%s", "网页控制台");
    format_capacity(0U, view.total_size, sizeof(view.total_size));
    format_capacity(0U, view.free_size, sizeof(view.free_size));
    return view;
}

esp_err_t web_console_presenter_init(void)
{
    const web_console_view_model_t initial = make_stopped_view();
    taskENTER_CRITICAL(&s_view_lock);
    if (!s_initialized)
    {
        s_view             = initial;
        s_applied_revision = 0U;
        s_initialized      = true;
    }
    taskEXIT_CRITICAL(&s_view_lock);
    return ESP_OK;
}

esp_err_t web_console_presenter_update_copy(const web_console_presenter_input_t *input, bool *out_accepted)
{
    if (input == NULL || out_accepted == NULL || input->presentation_revision == 0U
        || (int) input->state < (int) WEB_CONSOLE_PRESENTER_STATE_STOPPED || input->state > WEB_CONSOLE_PRESENTER_STATE_ERROR)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_accepted              = false;

    web_console_view_model_t view = {
        .state        = input->state,
        .running      = input->state == WEB_CONSOLE_PRESENTER_STATE_RUNNING,
        .exit_allowed = input->exit_allowed,
        .error        = input->error,
    };
    (void) snprintf(view.title, sizeof(view.title), "%s", title_for_state(input->state));
    format_capacity(input->total_bytes, view.total_size, sizeof(view.total_size));
    format_capacity(input->free_bytes, view.free_size, sizeof(view.free_size));

    if (view.running)
    {
        (void) memcpy(view.url, input->url, sizeof(view.url));
        view.url[sizeof(view.url) - 1U] = '\0';
        (void) memcpy(view.access_code, input->access_code, sizeof(view.access_code));
        view.access_code[sizeof(view.access_code) - 1U] = '\0';
    }

    taskENTER_CRITICAL(&s_view_lock);
    if (!s_initialized)
    {
        taskEXIT_CRITICAL(&s_view_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (input->presentation_revision > s_applied_revision)
    {
        s_view             = view;
        s_applied_revision = input->presentation_revision;
        *out_accepted      = true;
    }
    taskEXIT_CRITICAL(&s_view_lock);
    return ESP_OK;
}

void web_console_presenter_get_view_copy(web_console_view_model_t *out_view)
{
    if (out_view == NULL)
    {
        return;
    }

    taskENTER_CRITICAL(&s_view_lock);
    *out_view = s_view;
    taskEXIT_CRITICAL(&s_view_lock);
}
