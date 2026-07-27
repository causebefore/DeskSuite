idf_component_register(
    SRCS "src/time_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_netif esp_timer freertos
)
