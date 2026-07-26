idf_component_register(
    SRCS
        "utils.c"
        "task_stack_stats.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos log
)
