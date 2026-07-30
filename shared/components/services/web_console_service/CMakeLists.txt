set(web_file_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(web_file_generated_source "${web_file_generated_dir}/web_file_index.generated.c")
set(web_console_sources
    "${web_file_generated_source}"
    "src/core/web_console_service.cpp"
    "src/core/web_console_service_http.cpp"
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
    set(web_console_web_input "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
else()
    set(web_console_web_input "${CMAKE_CURRENT_SOURCE_DIR}/web/core.html")
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
    OUTPUT "${web_file_generated_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${web_file_generated_dir}"
    COMMAND "${python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            --input "${web_console_web_input}"
            --output "${web_file_generated_source}"
    DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            "${web_console_web_input}"
    VERBATIM
)
