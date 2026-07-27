idf_component_register(
    SRCS "src/deskmate_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common protocols
    PRIV_REQUIRES espressif__cjson transport
)
