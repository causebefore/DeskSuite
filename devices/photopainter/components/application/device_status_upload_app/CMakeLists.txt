idf_component_register(
    SRCS "src/device_status_upload_app.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES environment_service photopainter_protocol
)
