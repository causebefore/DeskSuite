idf_component_register(
    SRCS
        "src/content_refresh_app.cpp"
        "src/content_refresh_app_task.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common protocols
    PRIV_REQUIRES device_status_upload_app display_collection_service freertos network_manager remote_log sys utils
)
