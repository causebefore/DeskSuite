idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        connect
        network_manager
        protocols
        time_sync
        transport
        firmware_ota
        remote_log
        web_console_service
        web_console_network_provider
        utils
)
