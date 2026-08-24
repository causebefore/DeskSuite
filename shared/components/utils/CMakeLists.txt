idf_component_register(
    SRCS "utils.c" "libcrc.c" "utils_sha256.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES mbedtls
)
