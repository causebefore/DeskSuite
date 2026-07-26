idf_component_register(
    SRCS "pcf8563.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2c
)
