/*
 * 文件职责：把 Communication 网络快照转换为设置页 View Model。
 */
#include "settings_presenter.h"

#include <stdio.h>
#include <string.h>

#include "network_manager.h"
#include "settings_store.h"
#include "system_info.h"

static settings_view_model_t s_view;

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0)
    {
        return;
    }
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static void update_portal_view(settings_portal_view_model_t *out_view, const connect_portal_info_t *portal)
{
    if (out_view == NULL || portal == NULL)
    {
        return;
    }
    out_view->active = portal->active;
    copy_text(out_view->ssid, sizeof(out_view->ssid), portal->ssid);
    copy_text(out_view->password, sizeof(out_view->password), portal->password);
    copy_text(out_view->portal_url, sizeof(out_view->portal_url), portal->portal_url);
    copy_text(out_view->wifi_qr, sizeof(out_view->wifi_qr), portal->wifi_qr_payload);
    copy_text(out_view->url_qr, sizeof(out_view->url_qr), portal->portal_url);
}

static settings_network_view_state_t map_network_state(network_manager_state_t state)
{
    switch (state)
    {
        case NETWORK_STATE_CONNECTING:
        case NETWORK_STATE_RETRY_WAIT:
            return SETTINGS_NETWORK_VIEW_CONNECTING;
        case NETWORK_STATE_ONLINE:
            return SETTINGS_NETWORK_VIEW_CONNECTED;
        case NETWORK_STATE_ERROR:
            return SETTINGS_NETWORK_VIEW_FAILED;
        case NETWORK_STATE_PROVISIONING:
        case NETWORK_STATE_VALIDATING:
            return SETTINGS_NETWORK_VIEW_PORTAL;
        case NETWORK_STATE_STOPPED:
        case NETWORK_STATE_STOPPING:
        default:
            return SETTINGS_NETWORK_VIEW_IDLE;
    }
}

/**
 * @brief 从 Network Manager、Connect 与持久化设置刷新网络 View Model
 */
static void refresh_view(void)
{
    settings_view_model_t    next   = { 0 };
    network_manager_status_t status = { 0 };
    connect_link_info_t      link   = { 0 };
    connect_portal_info_t    portal = { 0 };

    if (network_manager_get_status_copy(&status) == ESP_OK)
    {
        next.network_state = map_network_state(status.state);
    }

    const bool link_ready = connect_get_link_snapshot_copy(&link) == ESP_OK;
    next.wifi_connected   = status.state == NETWORK_STATE_ONLINE && link_ready && link.associated && link.has_ipv4;
    if (link_ready)
    {
        copy_text(next.station_ssid, sizeof(next.station_ssid), link.ssid);
        copy_text(next.station_ip, sizeof(next.station_ip), link.has_ipv4 ? link.ip : "");
        next.rssi_dbm = link.rssi_dbm;
    }
    if (next.station_ssid[0] == '\0')
    {
        device_settings_t settings;
        if (settings_store_load_copy(&settings) == ESP_OK)
        {
            copy_text(next.station_ssid, sizeof(next.station_ssid), settings.wifi_ssid);
        }
    }

    if (network_manager_get_portal_info_copy(&portal) == ESP_OK)
    {
        update_portal_view(&next.portal, &portal);
        if (next.portal.active)
        {
            next.network_state = SETTINGS_NETWORK_VIEW_PORTAL;
        }
    }
    copy_text(next.current_version, sizeof(next.current_version), system_info_get_firmware_version_borrow());
    s_view = next;
}

esp_err_t settings_presenter_init(void)
{
    memset(&s_view, 0, sizeof(s_view));
    refresh_view();
    return ESP_OK;
}

void settings_presenter_get_view_copy(settings_view_model_t *out_view)
{
    if (out_view != NULL)
    {
        refresh_view();
        *out_view = s_view;
    }
}
