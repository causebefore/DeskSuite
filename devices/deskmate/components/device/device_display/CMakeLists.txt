idf_component_register(
    SRCS "src/device_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES bsp
)
