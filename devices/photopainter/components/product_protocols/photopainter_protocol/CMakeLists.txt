idf_component_register(
    SRCS
        "src/device_status_protocol.c"
        "src/display_frame_protocol.c"
        "src/display_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common transport
    PRIV_REQUIRES esp_timer espressif__cjson protocols utils
)
