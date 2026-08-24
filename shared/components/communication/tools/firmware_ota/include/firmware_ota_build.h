/**
 * @file firmware_ota_build.h
 * @brief 提供产品与 OTA 身份宏的组件默认值，并支持宿主工程注入覆盖。
 *
 * 宿主工程可在任意 include 路径提供 firmware_ota_build_project.h，
 * 在其中定义 DESKSUITE_PRODUCT_ID、DESKSUITE_FIRMWARE_TARGET 和
 * FIRMWARE_OTA_BUILD_VERSION；未定义的宏回落到本文件的默认值。
 * DeskSuite 设备工程由构建工具把该覆盖头生成到 build/generated 目录。
 */
#pragma once

#include <stdint.h>

#if __has_include("firmware_ota_build_project.h")
#include "firmware_ota_build_project.h"
#endif

/** @brief DeskSuite 产品 ID；默认 0 表示未注入真实产品身份 */
#ifndef DESKSUITE_PRODUCT_ID
#define DESKSUITE_PRODUCT_ID UINT32_C(0)
#endif

/** @brief 固件目标标识；默认空字符串表示未注入真实产品身份 */
#ifndef DESKSUITE_FIRMWARE_TARGET
#define DESKSUITE_FIRMWARE_TARGET ""
#endif

/** @brief 本次构建的 OTA 版本；默认 0 表示未注入真实产品身份 */
#ifndef FIRMWARE_OTA_BUILD_VERSION
#define FIRMWARE_OTA_BUILD_VERSION UINT64_C(0)
#endif
