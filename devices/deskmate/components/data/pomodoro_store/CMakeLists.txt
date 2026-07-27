idf_component_register(
    SRCS "src/pomodoro_store.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common nvs_flash
)
