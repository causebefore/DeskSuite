idf_component_register(
    SRCS "src/provisioning_app.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES button_service connect device_button device_display esp_timer freertos log
                  network_manager power_management_app sys utils
)
