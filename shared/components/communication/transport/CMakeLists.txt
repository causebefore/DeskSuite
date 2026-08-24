idf_component_register(
    SRCS "src/transport_http.cpp" "src/transport_websocket.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_client esp_timer esp_websocket_client freertos heap mbedtls
)
