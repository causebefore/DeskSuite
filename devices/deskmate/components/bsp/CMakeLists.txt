idf_component_register(
    SRCS
        "src/bsp_audio.c"
        "src/bsp_battery.c"
        "src/bsp_button.c"
        "src/bsp_display.c"
        "src/bsp_environment.c"
        "src/bsp_i2c.c"
        "src/bsp_power.c"
        "src/bsp_rtc.c"
        "src/bsp_storage.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES
        boards
        esp_adc
        esp_codec_dev
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_spi
        esp_hw_support
        esp_pm
        esp_timer
        freertos
        heap
        log
        pcf85063_driver
        sdmmc
        shtc3_driver
        utils
)
