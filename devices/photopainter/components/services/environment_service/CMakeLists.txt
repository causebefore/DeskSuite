idf_component_register(
    SRCS "src/environment_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES device_battery device_environment esp_timer freertos
)
