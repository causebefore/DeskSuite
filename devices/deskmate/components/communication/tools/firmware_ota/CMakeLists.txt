idf_component_register(
    SRCS "src/firmware_ota_task.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
    PRIV_REQUIRES app_update esp_system espressif__cjson freertos mbedtls protocols transport utils
)

set(DESKMATE_VERSION_FILE "${CMAKE_SOURCE_DIR}/version.txt")
set(FIRMWARE_OTA_BUILD_HEADER "${CMAKE_CURRENT_BINARY_DIR}/firmware_ota_build.h")
set(DESKMATE_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/deskmate_version.h")
set(FIRMWARE_OTA_BUILD_SCRIPT "${CMAKE_SOURCE_DIR}/tools/gen_ota_headers.cmake")

add_custom_target(gen_firmware_ota_build_headers ALL
    COMMAND "${CMAKE_COMMAND}"
            "-DVERSION_FILE=${DESKMATE_VERSION_FILE}"
            "-DFIRMWARE_OTA_OUT=${FIRMWARE_OTA_BUILD_HEADER}"
            "-DDESKMATE_VERSION_OUT=${DESKMATE_VERSION_HEADER}"
            -P "${FIRMWARE_OTA_BUILD_SCRIPT}"
    BYPRODUCTS "${FIRMWARE_OTA_BUILD_HEADER}" "${DESKMATE_VERSION_HEADER}"
    COMMENT "生成 DeskMate OTA 构建身份头文件"
)
add_dependencies(${COMPONENT_TARGET} gen_firmware_ota_build_headers)
