idf_component_register(
    SRCS
        "src/power_management_app.cpp"
        "src/power_management_app_task.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES content_refresh_app device_buzzer device_display device_led device_power device_sd esp_system firmware_ota freertos network_manager photo_playback_app remote_log sd_card_service sys
)
