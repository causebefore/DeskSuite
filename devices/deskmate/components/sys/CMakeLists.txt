idf_component_register(
    SRCS
        "settings_store.c"
        "system_clock.c"
        "system_filesystem.c"
        "system_info.c"
        "system_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES
        device_rtc
        device_storage
        esp_app_format
        esp_hw_support
        esp_psram
        esp_timer
        freertos
        fatfs
        heap
        log
        nvs_flash
        time_sync
)
