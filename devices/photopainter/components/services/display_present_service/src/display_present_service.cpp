/**
 * @file display_present_service.cpp
 * @brief 实现单张 PPF2 页面同步呈现事务
 */
#include "display_present_service.h"

#include <cstring>

#include "device_display.h"
#include "device_sd.h"
#include "display_frame_protocol.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "utils.h"

/** @brief 日志标签 */
static const char *TAG = "present_service";
/** @brief OTA 状态页等待现有物理刷新结束的最长时间 */
static constexpr uint32_t DISPLAY_PRESENT_STATUS_WAIT_MS = 20000U;

/** @brief 呈现 Service 进程期唯一 Runtime */
class DisplayPresentRuntime final
{
public:
    DisplayPresentRuntime() = default;
    DisplayPresentRuntime(const DisplayPresentRuntime &) = delete;
    DisplayPresentRuntime &operator=(const DisplayPresentRuntime &) = delete;
    DisplayPresentRuntime(DisplayPresentRuntime &&) = delete;
    DisplayPresentRuntime &operator=(DisplayPresentRuntime &&) = delete;

    bool initialized = false; /**< 生命周期初始化标记 */
    SemaphoreHandle_t operation_lock = nullptr; /**< 串行化呈现事务 */
    portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED; /**< 状态短临界区 */
    uint8_t *buffer = nullptr; /**< 单页 PSRAM 缓冲区 */
    display_present_service_status_t status = {}; /**< 对外状态 */
};

/** @brief 呈现 Service 唯一 Runtime */
static DisplayPresentRuntime s_runtime;

/**
 * @brief 发布呈现完成状态
 */
static void display_present_service_publish_result(const char *page_id, esp_err_t error)
{
    taskENTER_CRITICAL(&s_runtime.state_lock);
    s_runtime.status.busy       = false;
    s_runtime.status.last_error = error;
    if (error == ESP_OK && page_id != nullptr)
    {
        utils_copy_string(s_runtime.status.last_page_id,
                          sizeof(s_runtime.status.last_page_id),
                          page_id);
    }
    taskEXIT_CRITICAL(&s_runtime.state_lock);
}

esp_err_t display_present_service_init(void)
{
    if (s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    device_display_info_t display_info;
    esp_err_t error = device_display_get_info_copy(&display_info);
    if (error != ESP_OK)
    {
        return error;
    }
    if (display_info.mode != DEVICE_DISPLAY_MODE_GRAYSCALE_4
        || display_info.width_pixels != DISPLAY_FRAME_PROTOCOL_WIDTH
        || display_info.height_pixels != DISPLAY_FRAME_PROTOCOL_HEIGHT)
    {
        ESP_LOGE(TAG, "呈现 Service 要求 800x480 四灰阶显示模式");
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.operation_lock = xSemaphoreCreateMutex();
    if (s_runtime.operation_lock == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    s_runtime.buffer = static_cast<uint8_t *>(heap_caps_malloc(
        DISPLAY_FRAME_PROTOCOL_FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_runtime.buffer == nullptr)
    {
        vSemaphoreDelete(s_runtime.operation_lock);
        s_runtime.operation_lock = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_runtime.status      = {};
    s_runtime.initialized = true;
    return ESP_OK;
}

esp_err_t display_present_service_present_borrow(const char *file_path,
                                                 const display_protocol_page_t *expected_page)
{
    if (file_path == nullptr || file_path[0] == '\0' || expected_page == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_runtime.operation_lock, 0U) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_runtime.state_lock);
    s_runtime.status.busy = true;
    taskEXIT_CRITICAL(&s_runtime.state_lock);

    ESP_LOGI(TAG,
             "开始呈现页面: page_id=%s, version=%s, file=%s",
             expected_page->page_id,
             expected_page->content_version,
             file_path);

    size_t frame_size = 0U;
    esp_err_t error = device_sd_read_file(file_path,
                                         s_runtime.buffer,
                                         DISPLAY_FRAME_PROTOCOL_FILE_SIZE,
                                         &frame_size);
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "页面文件已从 SD 读入: page_id=%s, bytes=%u",
                 expected_page->page_id,
                 (unsigned int) frame_size);
    }
    display_frame_protocol_view_t frame_view;
    if (error == ESP_OK)
    {
        error = display_frame_protocol_validate_borrow(
            s_runtime.buffer, frame_size, expected_page, &frame_view);
    }
    if (error == ESP_OK)
    {
        device_display_image_view_t image = {};
        image.pixels        = frame_view.pixels;
        image.size_bytes    = frame_view.size_bytes;
        image.stride_bytes  = frame_view.stride_bytes;
        image.width_pixels  = frame_view.width;
        image.height_pixels = frame_view.height;
        image.pixel_format  = DEVICE_DISPLAY_PIXEL_FORMAT_GRAY_2BPP;
        error = device_display_blit_borrow(0U, 0U, &image);
    }
    if (error == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "页面数据校验与帧缓冲写入完成，开始刷新墨水屏: page_id=%s",
                 expected_page->page_id);
        error = device_display_present();
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "呈现页面 %s 失败: %s", expected_page->page_id, esp_err_to_name(error));
    }
    else
    {
        ESP_LOGI(TAG, "墨水屏物理刷新完成: page_id=%s", expected_page->page_id);
    }
    display_present_service_publish_result(expected_page->page_id, error);
    xSemaphoreGive(s_runtime.operation_lock);
    return error;
}

esp_err_t display_present_service_present_ascii_centered_borrow(const char *text, uint8_t scale)
{
    if (text == nullptr || text[0] == '\0' || scale == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_runtime.operation_lock,
                       pdMS_TO_TICKS(DISPLAY_PRESENT_STATUS_WAIT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_runtime.state_lock);
    s_runtime.status.busy = true;
    taskEXIT_CRITICAL(&s_runtime.state_lock);

    ESP_LOGI(TAG, "开始呈现居中 ASCII 状态文本: %s", text);
    const esp_err_t error = device_display_present_ascii_centered_borrow(text, scale);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "呈现居中 ASCII 状态文本失败: %s", esp_err_to_name(error));
    }
    else
    {
        ESP_LOGI(TAG, "居中 ASCII 状态文本物理刷新完成");
    }
    display_present_service_publish_result(nullptr, error);
    xSemaphoreGive(s_runtime.operation_lock);
    return error;
}

esp_err_t display_present_service_present_ascii_layout_borrow(
    const display_present_service_ascii_line_t *lines, size_t line_count)
{
    if (lines == nullptr || line_count == 0U
        || line_count > DISPLAY_PRESENT_SERVICE_ASCII_LINE_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < line_count; ++index)
    {
        if (lines[index].text == nullptr || lines[index].text[0] == '\0'
            || lines[index].scale == 0U)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_runtime.operation_lock,
                       pdMS_TO_TICKS(DISPLAY_PRESENT_STATUS_WAIT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    taskENTER_CRITICAL(&s_runtime.state_lock);
    s_runtime.status.busy = true;
    taskEXIT_CRITICAL(&s_runtime.state_lock);

    ESP_LOGI(TAG, "开始呈现 %u 行坐标式 ASCII 状态页", (unsigned int) line_count);
    esp_err_t error = device_display_clear(DEVICE_DISPLAY_TONE_WHITE);
    for (size_t index = 0U; error == ESP_OK && index < line_count; ++index)
    {
        error = device_display_draw_ascii_borrow(lines[index].x_pixels,
                                                 lines[index].y_pixels,
                                                 lines[index].text,
                                                 lines[index].scale);
    }
    if (error == ESP_OK)
    {
        error = device_display_present();
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "呈现坐标式 ASCII 状态页失败: %s", esp_err_to_name(error));
    }
    else
    {
        ESP_LOGI(TAG, "坐标式 ASCII 状态页物理刷新完成");
    }
    display_present_service_publish_result(nullptr, error);
    xSemaphoreGive(s_runtime.operation_lock);
    return error;
}

esp_err_t display_present_service_get_status_copy(display_present_service_status_t *out_status)
{
    if (out_status == nullptr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_runtime.state_lock);
    *out_status = s_runtime.status;
    taskEXIT_CRITICAL(&s_runtime.state_lock);
    return ESP_OK;
}

esp_err_t display_present_service_deinit(void)
{
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_runtime.operation_lock, 0U) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.initialized = false;
    heap_caps_free(s_runtime.buffer);
    s_runtime.buffer = nullptr;
    s_runtime.status = {};
    xSemaphoreGive(s_runtime.operation_lock);
    vSemaphoreDelete(s_runtime.operation_lock);
    s_runtime.operation_lock = nullptr;
    return ESP_OK;
}
