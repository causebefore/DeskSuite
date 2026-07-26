idf_component_register(
    SRCS "src/device_battery.c" "src/device_battery_logic.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES bsp freertos
)
