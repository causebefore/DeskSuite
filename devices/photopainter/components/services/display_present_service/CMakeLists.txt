idf_component_register(
    SRCS "src/display_present_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common protocols
    PRIV_REQUIRES device_display device_sd freertos heap utils
)
