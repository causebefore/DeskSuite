idf_component_register(
    SRCS "src/voice_service.cpp" "src/voice_service_task.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        esp_event
        protocols
    PRIV_REQUIRES
        audio_processor_service
        audio_service
        esp_timer
        freertos
        heap
        log
        transport
)
