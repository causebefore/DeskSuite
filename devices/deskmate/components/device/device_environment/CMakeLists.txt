idf_component_register(
    SRCS "src/device_environment.c" "src/device_environment_logic.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES bsp freertos
)
