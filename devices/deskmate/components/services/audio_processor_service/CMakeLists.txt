idf_component_register(
    SRCS "src/audio_processor_service.c" "src/wake_arbiter_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_event
    PRIV_REQUIRES
        audio_service
        esp-sr
        espressif__esp_audio_effects
        freertos
        heap
        log
        utils
)
