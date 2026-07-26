idf_component_register(
    SRCS
        "src/connect.cpp"
        "src/connect_portal.c"
        "src/connect_portal_dns.c"
        "src/connect_portal_dns_task.c"
        "src/connect_portal_form.c"
        "src/connect_portal_qr.c"
        "src/connect_portal_scan_task.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common freertos esp_wifi esp_event esp_netif esp_http_server lwip
    PRIV_REQUIRES utils espressif__qrcode
)
