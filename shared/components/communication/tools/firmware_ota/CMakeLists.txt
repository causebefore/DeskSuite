# 宿主构建目录存在 generated 时（DeskSuite 工程由构建工具提前创建），
# 把它加入私有 include 路径，firmware_ota_build.h 会拾取其中的身份覆盖头。
set(firmware_ota_extra_priv_include_args)
if(EXISTS "${CMAKE_BINARY_DIR}/generated")
    set(firmware_ota_extra_priv_include_args PRIV_INCLUDE_DIRS "${CMAKE_BINARY_DIR}/generated")
endif()

idf_component_register(
    SRCS "src/firmware_ota_task.c"
    INCLUDE_DIRS "include"
    ${firmware_ota_extra_priv_include_args}
    REQUIRES esp_common protocols
    PRIV_REQUIRES app_update esp_system espressif__cjson freertos mbedtls transport utils
)
