/*
 * 文件职责：编排设置菜单发起的手动 OTA 检查、确认安装和目标清理。
 */
#include "app_ota.h"

#include "app_network.h"
#include "ota_presenter.h"

esp_err_t app_ota_init(void)
{
    /* 当前产品仍只开放菜单内手动检查；显式自动模式留给周期策略启用时使用。 */
    app_network_set_ota_auto_check_enabled(false);
    return ESP_OK;
}

esp_err_t app_ota_check(void)
{
    ota_presenter_show_checking();
    esp_err_t error = app_network_discard_ota_update();
    if (error == ESP_OK)
    {
        error = app_network_request_ota_check(APP_NETWORK_OTA_CHECK_MANUAL);
    }
    if (error != ESP_OK)
    {
        ota_presenter_show_check_request_failed(error, true);
    }
    return error;
}

esp_err_t app_ota_install(void)
{
    const esp_err_t error = app_network_request_ota_install();
    if (error == ESP_OK)
    {
        ota_presenter_show_downloading();
    }
    else
    {
        ota_presenter_show_install_submit_failed(error);
    }
    return error;
}

esp_err_t app_ota_discard_pending_update(void)
{
    const esp_err_t error = app_network_discard_ota_update();
    if (error == ESP_OK)
    {
        ota_presenter_reset();
    }
    else
    {
        ota_presenter_show_discard_failed(error);
    }
    return error;
}

bool app_ota_is_navigation_locked(void)
{
    return app_network_is_ota_busy();
}
