set(web_file_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(web_file_generated_source "${web_file_generated_dir}/web_file_index.generated.c")

idf_component_register(
    SRCS
        "${web_file_generated_source}"
        "src/web_console_service.cpp"
        "src/web_console_service_http.cpp"
        "src/web_console_service_auth.cpp"
        "src/web_console_service_path.cpp"
        "src/web_console_service_transfer.cpp"
        "src/web_console_service_read.cpp"
        "src/web_console_service_mutation.cpp"
        "src/web_console_service_transaction.cpp"
        "src/web_console_service_stop_task.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        esp_http_server
        esp_system
        esp_timer
        freertos
        heap
        log
        sys
)

idf_build_get_property(python PYTHON)
add_custom_command(
    OUTPUT "${web_file_generated_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${web_file_generated_dir}"
    COMMAND "${python}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            --input "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            --output "${web_file_generated_source}"
    DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_html.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    VERBATIM
)
