set(protocol_sources
    "src/device_status_protocol.c"
    "src/display_frame_protocol.c"
    "src/display_protocol.c"
    "src/log_upload.c"
    "src/protocol_identity.c"
    "src/voice_protocol.c"
    "src/protocol_url.c"
)

idf_component_register(
    SRCS ${protocol_sources}
    INCLUDE_DIRS "include"
    REQUIRES esp_common transport
    PRIV_REQUIRES esp_timer espressif__cjson heap utils
)
