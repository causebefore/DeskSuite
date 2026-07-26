idf_component_register(
    SRCS "src/audio_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES device_audio freertos log
)
