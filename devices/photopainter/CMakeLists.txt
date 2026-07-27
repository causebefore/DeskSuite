# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/application/bootstart_app"
    "${CMAKE_CURRENT_LIST_DIR}/components/application/content_refresh_app"
    "${CMAKE_CURRENT_LIST_DIR}/components/application/device_status_upload_app"
    "${CMAKE_CURRENT_LIST_DIR}/components/application/photo_playback_app"
    "${CMAKE_CURRENT_LIST_DIR}/components/application/power_management_app"
    "${CMAKE_CURRENT_LIST_DIR}/components/application/provisioning_app"
    "${CMAKE_CURRENT_LIST_DIR}/components/boards"
    "${CMAKE_CURRENT_LIST_DIR}/components/bsp"
    "${CMAKE_CURRENT_LIST_DIR}/components/product_protocols/photopainter_protocol"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/components/communication/connect"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/components/communication/network_manager"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/components/communication/protocols"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/components/communication/time_sync"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/components/communication/transport"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/components/communication/tools/firmware_ota"
    "${CMAKE_CURRENT_LIST_DIR}/../../shared/components/communication/tools/remote_log"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_battery"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_button"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_buzzer"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_display"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_environment"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_led"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_power"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_rtc"
    "${CMAKE_CURRENT_LIST_DIR}/components/device/device_sd"
    "${CMAKE_CURRENT_LIST_DIR}/components/drivers/button_driver"
    "${CMAKE_CURRENT_LIST_DIR}/components/drivers/uc8179"
    "${CMAKE_CURRENT_LIST_DIR}/components/drivers/pcf8563"
    "${CMAKE_CURRENT_LIST_DIR}/components/drivers/sht4x"
    "${CMAKE_CURRENT_LIST_DIR}/components/services/button_service"
    "${CMAKE_CURRENT_LIST_DIR}/components/services/display_collection_service"
    "${CMAKE_CURRENT_LIST_DIR}/components/services/display_present_service"
    "${CMAKE_CURRENT_LIST_DIR}/components/services/environment_service"
    "${CMAKE_CURRENT_LIST_DIR}/components/services/sd_card_service"
    "${CMAKE_CURRENT_LIST_DIR}/components/sys"
    "${CMAKE_CURRENT_LIST_DIR}/components/utils"
)

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" PHOTOPAINTER_APP_VERSION)
string(STRIP "${PHOTOPAINTER_APP_VERSION}" PHOTOPAINTER_APP_VERSION)
set(PROJECT_VER "${PHOTOPAINTER_APP_VERSION}")
project(PhotoPainter_Device VERSION ${PHOTOPAINTER_APP_VERSION})
