set(protocol_sources
    "src/deskmate_api.c"
    "src/log_upload.c"
    "src/protocol_identity.c"
    "src/protocol_url.c"
    "src/voice_protocol.c"
)

idf_component_register(
    SRCS ${protocol_sources}
    INCLUDE_DIRS "include"
    REQUIRES esp_common transport
    PRIV_REQUIRES esp_timer espressif__cjson heap
)
