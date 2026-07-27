/**
 * @file main.c
 * @brief PhotoPainter 固件入口
 */

#include "bootstart_app.h"

#include "esp_log.h"

void app_main(void)
{
    bootstart_app_wakeup_context_t wakeup_context = { 0 };
    esp_err_t startup_error = bootstart_app_get_wakeup_context_copy(&wakeup_context);
    if (startup_error != ESP_OK)
    {
        ESP_LOGE("main", "读取启动唤醒上下文失败: %s", esp_err_to_name(startup_error));
        bootstart_app_handle_fatal_error(startup_error);
    }

    startup_error = bootstart_app_init_system_services();
    if (startup_error != ESP_OK)
    {
        bootstart_app_handle_fatal_error(startup_error);
    }

    (void) bootstart_app_init_rtc();
    const esp_err_t wakeup_gate_error = bootstart_app_enforce_absolute_wakeup_gate(&wakeup_context);
    if (wakeup_gate_error != ESP_OK)
    {
        ESP_LOGW("main",
                 "启动绝对目标时间门禁未能生效，继续启动以便联网恢复: %s",
                 esp_err_to_name(wakeup_gate_error));
    }

    ESP_LOGI("main", "========== 本轮设备工作流程开始 ==========");
    (void) bootstart_app_start_environment();
    (void) bootstart_app_start_sd();

    startup_error = bootstart_app_init_local_communication();
    if (startup_error != ESP_OK)
    {
        bootstart_app_handle_fatal_error(startup_error);
    }
    startup_error = bootstart_app_run_provisioning(&wakeup_context);
    if (startup_error != ESP_OK)
    {
        bootstart_app_handle_fatal_error(startup_error);
    }

    ESP_LOGI("main", "网络准备完成，开始装配照片同步与显示链路");
    startup_error = bootstart_app_start_photo_pipeline();
    if (startup_error != ESP_OK)
    {
        bootstart_app_handle_fatal_error(startup_error);
    }

    bootstart_app_init_feedback_devices();
    startup_error = bootstart_app_start_power_management();
    if (startup_error != ESP_OK)
    {
        bootstart_app_handle_fatal_error(startup_error);
    }
    startup_error = bootstart_app_start_content_refresh();
    if (startup_error != ESP_OK)
    {
        bootstart_app_handle_fatal_error(startup_error);
    }
}
