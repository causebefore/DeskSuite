set(web_console_network_provider_sources)
if(CONFIG_WEB_CONSOLE_STATUS)
    list(APPEND web_console_network_provider_sources
        "web_console_network_provider.c"
    )
endif()

idf_component_register(
    SRCS ${web_console_network_provider_sources}
    INCLUDE_DIRS
        "include"
    REQUIRES
        web_console_service
    PRIV_REQUIRES
        network_manager
)
