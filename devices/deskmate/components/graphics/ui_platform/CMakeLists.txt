idf_component_register(
    SRCS
        "lvgl/lvgl_runtime.c"
        "fonts/font_runtime.c"
        "fonts/font_backend.c"
        "fonts/bin_font_lvgl_adapter.c"
        "fonts/bin_font_probe.c"
        "fonts/rlcd_font_container.c"
        "images/image_runtime.c"
        "images/generated/qweather_i1_assets.c"
        "images/generated/status_i1_assets.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "fonts" "images" "images/generated"
    REQUIRES
        device_display
        esp_common
        esp_lvgl_port
        esp_partition
        esp_timer
        freertos
        heap
        log
        lvgl
        spi_flash
        sys
)
