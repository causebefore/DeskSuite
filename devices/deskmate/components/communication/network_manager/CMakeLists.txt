idf_component_register(
    SRCS
        "src/network_manager.cpp"
        "src/network_manager_task.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES connect esp_common
    PRIV_REQUIRES freertos utils
)
