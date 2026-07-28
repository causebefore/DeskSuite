set(connect_portal_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(connect_portal_generated_source "${connect_portal_generated_dir}/connect_portal_web.generated.c")

idf_component_register(
    SRCS
        "${connect_portal_generated_source}"
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

idf_build_get_property(python PYTHON)
add_custom_command(
    OUTPUT "${connect_portal_generated_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${connect_portal_generated_dir}"
    COMMAND "${python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            --index "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            --success "${CMAKE_CURRENT_SOURCE_DIR}/web/success.html"
            --output "${connect_portal_generated_source}"
            --default-service-url "${CONFIG_CONNECT_PORTAL_DEFAULT_SERVICE_URL}"
    DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/web/success.html"
    VERBATIM
)
