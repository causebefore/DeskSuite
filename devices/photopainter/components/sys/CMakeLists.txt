idf_component_register(
    SRCS "system_clock.c" "system_periodic_timer.c" "system_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES device_rtc esp_timer freertos nvs_flash time_sync
)
