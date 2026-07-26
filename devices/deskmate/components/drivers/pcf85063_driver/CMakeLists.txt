idf_component_register(
    SRCS "src/pcf85063_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2c
    PRIV_REQUIRES log
)
