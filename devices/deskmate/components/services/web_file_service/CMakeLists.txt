set(web_file_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(web_file_generated_source "${web_file_generated_dir}/web_file_index.generated.c")

idf_component_register(
    SRCS
        "${web_file_generated_source}"
        "src/web_file_service.c"
        "src/web_file_service_auth.c"
        "src/web_file_service_path.c"
        "src/web_file_service_transfer.c"
        "src/web_file_service_transaction.c"
        "src/web_file_service_stop_task.c"
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
