idf_component_register(
    SRCS "src/firmware_ota_task.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_BINARY_DIR}/generated"
    PRIV_REQUIRES app_update esp_system espressif__cjson freertos mbedtls protocols transport utils
)
