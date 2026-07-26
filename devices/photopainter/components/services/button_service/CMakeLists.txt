idf_component_register(
    SRCS "src/button_service.c"
    INCLUDE_DIRS "include"
    REQUIRES device_button esp_common
    PRIV_REQUIRES sys
)
