# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    components/boards
    components/json
    components/drivers/pcf85063_driver
    components/drivers/shtc3_driver
    components/bsp
    components/device/device_audio
    components/device/device_battery
    components/device/device_button
    components/device/device_display
    components/device/device_environment
    components/device/device_power
    components/device/device_rtc
    components/device/device_storage
    components/utils
    components/sys
    components/communication/connect
    components/communication/transport
    components/communication/protocols
    components/communication/network_manager
    components/communication/tools/firmware_ota
    components/communication/tools/remote_log
    components/services/audio_service
    components/services/audio_processor_service
    components/services/button_service
    components/services/environment_service
    components/services/rtc_service
    components/services/voice_service
    components/services/web_file_service
    components/graphics/ui_platform
    components/data/weather
    components/data/calendar
    components/data/dashboard_store
    components/data/mail
    components/data/quota
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(GROUP_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/boards"
    "${CMAKE_CURRENT_LIST_DIR}/components/drivers"
    "${CMAKE_CURRENT_LIST_DIR}/components/bsp"
    "${CMAKE_CURRENT_LIST_DIR}/components/communication"
    "${CMAKE_CURRENT_LIST_DIR}/components/sys"
    "${CMAKE_CURRENT_LIST_DIR}/components/device"
    "${CMAKE_CURRENT_LIST_DIR}/components/graphics"
    "${CMAKE_CURRENT_LIST_DIR}/components/services"
    "${CMAKE_CURRENT_LIST_DIR}/components/data"
    "${CMAKE_CURRENT_LIST_DIR}/components/graphics/ui_platform"
)
idf_build_set_property(EXTRA_COMPONENT_EXCLUDE_DIRS "${GROUP_COMPONENT_DIRS}")
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 注册 esp-sr model 分区的自定义 SubType（IDF v6 分区表解析器需要）
idf_build_set_property(EXTRA_PARTITION_SUBTYPES "data,model,0x41")

file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" DESKMATE_APP_VERSION)
string(STRIP "${DESKMATE_APP_VERSION}" DESKMATE_APP_VERSION)
# OTA 版本号（裸版本+时间戳）由 firmware_ota 组件在 build 期生成 deskmate_version.h，
# 每次 build 自动刷新，脱离 configure 期 PROJECT_VER 限制。
set(PROJECT_VER "${DESKMATE_APP_VERSION}")
project(esp32 VERSION ${DESKMATE_APP_VERSION})

# esp-dsp 1.7.0 的 EKF 源文件使用 std::sin/cos/atan，但未直接包含 <cmath>。
# 仅对该组件的 C++ 编译强制包含，兼容 ESP-IDF 6.0.1 的 GNU++26 工具链。
idf_component_get_property(esp_dsp_lib espressif__esp-dsp COMPONENT_LIB)
target_compile_options(${esp_dsp_lib} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-include;cmath>")

# Workaround: GCC esp-15.2.0 在 -Og 下编译 esp_lcd_panel_rgb.c 触发内部段错误。
# 对 esp_lcd 组件单独提升优化级别到 -O2 绕过编译器 bug。
target_compile_options(__idf_esp_lcd PRIVATE -O2)
