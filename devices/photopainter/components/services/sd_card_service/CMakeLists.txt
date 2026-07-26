idf_component_register(
    SRCS
        "src/sd_card_service.c"
        "src/sd_card_service_task.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES device_sd freertos
)
