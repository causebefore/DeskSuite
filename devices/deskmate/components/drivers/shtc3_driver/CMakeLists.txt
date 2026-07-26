idf_component_register(
    SRCS "src/shtc3_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2c
    PRIV_REQUIRES freertos
)
