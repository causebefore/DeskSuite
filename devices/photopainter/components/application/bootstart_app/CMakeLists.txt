idf_component_register(
    SRCS "bootstart_app.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES
        button_service
        content_refresh_app
        device_battery
        device_buzzer
        device_button
        device_display
        device_environment
        device_led
        device_power
        device_rtc
        device_sd
        display_collection_service
        display_present_service
        environment_service
        firmware_ota
        log
        network_manager
        photo_playback_app
        power_management_app
        provisioning_app
        remote_log
        sd_card_service
        sys
)
