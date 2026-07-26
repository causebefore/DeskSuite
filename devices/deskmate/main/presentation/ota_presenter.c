/*
 * 文件职责：把 Firmware OTA 完成事实转换为设置页可读取的线程安全快照。
 */
#include "ota_presenter.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "presentation_dispatch.h"
#include "system_info.h"

static portMUX_TYPE               s_view_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_presenter_view_model_t s_view;

/** @brief 安全复制可能为空的短文本 */
static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0U)
    {
        return;
    }
    (void) snprintf(destination, capacity, "%s", source != NULL ? source : "");
}

/** @brief 采集当前固件版本到调用方提供的固定缓冲区 */
static void copy_current_version(char out_version[FIRMWARE_OTA_VERSION_MAX])
{
    copy_text(out_version, FIRMWARE_OTA_VERSION_MAX, system_info_get_firmware_version_borrow());
}

/** @brief 通知 UI 重新读取 OTA View Model */
static void notify_view_changed(void)
{
    (void) presentation_dispatch_ota_update();
}

/** @brief 构造不含待安装目标的基础快照 */
static ota_presenter_view_model_t make_base_view(ota_presenter_state_t state, esp_err_t error)
{
    ota_presenter_view_model_t view = {
        .state = state,
        .error = error,
    };
    copy_current_version(view.current_version);
    return view;
}

esp_err_t ota_presenter_init(void)
{
    const ota_presenter_view_model_t initial = make_base_view(OTA_PRESENTER_STATE_IDLE, ESP_OK);
    taskENTER_CRITICAL(&s_view_lock);
    s_view = initial;
    taskEXIT_CRITICAL(&s_view_lock);
    return ESP_OK;
}

void ota_presenter_show_checking(void)
{
    const ota_presenter_view_model_t checking = make_base_view(OTA_PRESENTER_STATE_CHECKING, ESP_OK);
    taskENTER_CRITICAL(&s_view_lock);
    s_view = checking;
    taskEXIT_CRITICAL(&s_view_lock);
    notify_view_changed();
}

void ota_presenter_show_downloading(void)
{
    taskENTER_CRITICAL(&s_view_lock);
    s_view.state = OTA_PRESENTER_STATE_DOWNLOADING;
    s_view.error = ESP_OK;
    taskEXIT_CRITICAL(&s_view_lock);
    notify_view_changed();
}

void ota_presenter_show_check_request_failed(esp_err_t error, bool manual)
{
    const ota_presenter_state_t      state  = manual ? OTA_PRESENTER_STATE_CHECK_FAILED : OTA_PRESENTER_STATE_IDLE;
    const ota_presenter_view_model_t failed = make_base_view(state, manual ? error : ESP_OK);
    taskENTER_CRITICAL(&s_view_lock);
    s_view = failed;
    taskEXIT_CRITICAL(&s_view_lock);
    notify_view_changed();
}

void ota_presenter_show_install_submit_failed(esp_err_t error)
{
    taskENTER_CRITICAL(&s_view_lock);
    s_view.state = OTA_PRESENTER_STATE_AVAILABLE;
    s_view.error = error;
    taskEXIT_CRITICAL(&s_view_lock);
    notify_view_changed();
}

void ota_presenter_show_discard_failed(esp_err_t error)
{
    ota_presenter_show_install_submit_failed(error);
}

void ota_presenter_reset(void)
{
    const ota_presenter_view_model_t idle = make_base_view(OTA_PRESENTER_STATE_IDLE, ESP_OK);
    taskENTER_CRITICAL(&s_view_lock);
    s_view = idle;
    taskEXIT_CRITICAL(&s_view_lock);
    notify_view_changed();
}

esp_err_t ota_presenter_get_view_copy(ota_presenter_view_model_t *out_view)
{
    if (out_view == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_view_lock);
    *out_view = s_view;
    taskEXIT_CRITICAL(&s_view_lock);
    return ESP_OK;
}

void ota_presenter_handle_firmware_event_copy(const firmware_ota_event_t *event, bool manual)
{
    if (event == NULL)
    {
        return;
    }

    if (event->type == FIRMWARE_OTA_EVENT_CHECK_COMPLETED)
    {
        ota_presenter_view_model_t result;
        if (event->result != ESP_OK)
        {
            result = make_base_view(manual ? OTA_PRESENTER_STATE_CHECK_FAILED : OTA_PRESENTER_STATE_IDLE,
                                    manual ? event->result : ESP_OK);
        }
        else if (!event->check_result.update_available)
        {
            result = make_base_view(manual ? OTA_PRESENTER_STATE_NO_UPDATE : OTA_PRESENTER_STATE_IDLE, ESP_OK);
        }
        else
        {
            result = make_base_view(OTA_PRESENTER_STATE_AVAILABLE, ESP_OK);
            copy_text(result.target_version, sizeof(result.target_version), event->check_result.target_version);
            result.target_size_bytes = event->check_result.target_size;
        }

        taskENTER_CRITICAL(&s_view_lock);
        s_view = result;
        taskEXIT_CRITICAL(&s_view_lock);
        notify_view_changed();
        return;
    }

    if (event->result == ESP_OK)
    {
        /* Firmware OTA 会在完成回调返回后立即重启；这里只保留下载态避免闪回。 */
        notify_view_changed();
        return;
    }

    taskENTER_CRITICAL(&s_view_lock);
    s_view.state = OTA_PRESENTER_STATE_INSTALL_FAILED;
    s_view.error = event->result;
    taskEXIT_CRITICAL(&s_view_lock);
    notify_view_changed();
}
