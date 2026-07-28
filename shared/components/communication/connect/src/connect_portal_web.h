/*
 * 文件职责：声明由配网 HTML 构建脚本生成的只读 gzip 页面资源。
 * 主要依赖：标准整数和大小类型。
 * 调用方：connect_portal.c、构建目录中的生成源码。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

extern const uint8_t connect_portal_index_gzip[];
extern const size_t  connect_portal_index_gzip_size_bytes;
extern const uint8_t connect_portal_success_gzip[];
extern const size_t  connect_portal_success_gzip_size_bytes;
