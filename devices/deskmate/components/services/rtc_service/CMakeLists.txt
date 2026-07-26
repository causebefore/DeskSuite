idf_component_register(
    SRCS "src/rtc_service_task.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES device_rtc freertos log utils
)
