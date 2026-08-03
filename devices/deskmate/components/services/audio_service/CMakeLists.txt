idf_component_register(
    SRCS "src/audio_service.cpp" "src/audio_service_playback_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_event
    PRIV_REQUIRES
        device_audio
        espressif__esp_audio_codec
        espressif__esp_audio_effects
        freertos
        heap
        log
)
