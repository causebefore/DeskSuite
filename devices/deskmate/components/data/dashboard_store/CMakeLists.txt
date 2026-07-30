idf_component_register(
    SRCS "src/dashboard_store.c"
    INCLUDE_DIRS "include"
    REQUIRES deskmate_protocol esp_common freertos
)
