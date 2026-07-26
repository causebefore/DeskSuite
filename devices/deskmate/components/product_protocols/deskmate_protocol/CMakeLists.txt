idf_component_register(
    SRCS "src/deskmate_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES espressif__cjson protocols transport
)
