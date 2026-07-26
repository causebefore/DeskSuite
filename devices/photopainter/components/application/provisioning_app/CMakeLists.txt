idf_component_register(
    SRCS "src/provisioning_app.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES connect device_display freertos log network_manager power_management_app
)
