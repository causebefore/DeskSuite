idf_component_register(
    SRCS
        "src/photo_playback_app.cpp"
        "src/photo_playback_app_task.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common photopainter_protocol
    PRIV_REQUIRES button_service display_collection_service display_present_service esp_timer freertos utils
)
