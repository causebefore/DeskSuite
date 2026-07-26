idf_component_register(
    SRCS
        "src/display_collection_service.cpp"
        "src/display_collection_service_storage.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common protocols
    PRIV_REQUIRES device_sd espressif__cjson freertos heap utils
)
