idf_component_register(
    SRCS
        "src/remote_log.c"
        "src/remote_log_task.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES esp_app_format esp_system freertos log network_manager protocols
)

if(NOT CONFIG_LOG_VERSION_2)
    message(FATAL_ERROR "remote_log 仅支持 ESP-IDF Log V2，请启用 CONFIG_LOG_VERSION_2")
endif()
if(NOT CONFIG_LOG_MODE_TEXT)
    message(FATAL_ERROR "remote_log 仅支持 Log V2 文本模式，请启用 CONFIG_LOG_MODE_TEXT")
endif()

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_log")
