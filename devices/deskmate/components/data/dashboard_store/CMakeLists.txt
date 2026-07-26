idf_component_register(
    SRCS "src/dashboard_store.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos espressif__cjson
)
