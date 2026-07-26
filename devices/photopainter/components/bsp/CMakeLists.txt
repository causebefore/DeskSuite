idf_component_register(
    SRCS
        "src/bsp_battery.c"
        "src/bsp_buttons.c"
        "src/bsp_sd.c"
        "src/bsp_buzzer.c"
        "src/bsp_environment.c"
        "src/bsp_epaper.c"
        "src/bsp_epaper_ascii.c"
        "src/bsp_i2c.c"
        "src/bsp_led.c"
        "src/bsp_power.c"
        "src/bsp_rtc.c"
        "src/bsp_spi.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES boards button_driver esp_adc esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi esp_hw_support esp_system fatfs heap pcf8563 sht4x uc8179
)
