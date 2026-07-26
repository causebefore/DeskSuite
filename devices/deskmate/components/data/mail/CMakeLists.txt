idf_component_register(
    SRCS "src/mail.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_event freertos dashboard_store
)
