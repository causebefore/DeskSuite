#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 经 gzip 压缩的网页前端静态资源 */
extern const uint8_t web_console_index_gz[];
/** @brief gzip 压缩后的前端资源字节数 */
extern const size_t  web_console_index_gz_size;

#ifdef __cplusplus
}
#endif
