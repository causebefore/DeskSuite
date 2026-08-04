set(web_console_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(web_console_generated_source "${web_console_generated_dir}/web_console_index.generated.c")
set(web_console_sources
    "${web_console_generated_source}"
    "src/core/web_console_service.cpp"
    "src/core/web_console_service_http.cpp"
    "src/core/web_console_http_common.cpp"
    "src/core/web_console_service_auth.cpp"
    "src/core/web_console_service_stop_task.cpp"
)
set(web_console_private_include_dirs "src/core")
set(web_console_private_requires
    esp_http_server
    esp_system
    esp_timer
    freertos
    log
)
set(web_console_web_modules)
set(web_console_web_dependencies
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/common.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/common.js"
)

if(CONFIG_WEB_CONSOLE_FILES)
    list(APPEND web_console_sources
        "src/files/web_console_files.cpp"
        "src/files/web_console_service_path.cpp"
        "src/files/web_console_service_transfer.cpp"
        "src/files/web_console_service_read.cpp"
        "src/files/web_console_service_mutation.cpp"
        "src/files/web_console_service_transaction.cpp"
    )
    list(APPEND web_console_private_include_dirs "src/files")
    list(APPEND web_console_private_requires heap)
    list(APPEND web_console_web_modules --module files)
    list(APPEND web_console_web_dependencies
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/files.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/files.css"
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/files.js"
    )
endif()

if(CONFIG_WEB_CONSOLE_SETTINGS OR CONFIG_WEB_CONSOLE_STATUS)
    list(APPEND web_console_sources
        "src/providers/web_console_provider_registry.cpp"
        "src/providers/web_console_provider_http.cpp"
    )
    list(APPEND web_console_private_include_dirs "src/providers")
    list(APPEND web_console_private_requires espressif__cjson)
    list(APPEND web_console_web_dependencies
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/fields.css"
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/fields.js"
    )
endif()

if(CONFIG_WEB_CONSOLE_SETTINGS)
    list(APPEND web_console_web_modules --module settings)
    list(APPEND web_console_web_dependencies
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/settings.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/settings.js"
    )
endif()

if(CONFIG_WEB_CONSOLE_STATUS)
    list(APPEND web_console_web_modules --module status)
    list(APPEND web_console_web_dependencies
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/status.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/web/modules/status.js"
    )
endif()

idf_component_register(
    SRCS ${web_console_sources}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS ${web_console_private_include_dirs}
    REQUIRES
        esp_common
    PRIV_REQUIRES ${web_console_private_requires}
)

idf_build_get_property(python PYTHON)
add_custom_command(
    OUTPUT "${web_console_generated_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${web_console_generated_dir}"
    COMMAND "${python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            --input "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            --output "${web_console_generated_source}"
            ${web_console_web_modules}
    DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            ${web_console_web_dependencies}
    VERBATIM
)
