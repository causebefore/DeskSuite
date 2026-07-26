# DeskMate OTA 构建身份生成脚本。
# 每次构建生成诊断版本和单调可比较的 UTC 时间版本号。

file(READ "${VERSION_FILE}" BASE_VERSION)
string(STRIP "${BASE_VERSION}" BASE_VERSION)
string(TIMESTAMP BUILD_NUMBER "%Y%m%d%H%M%S" UTC)
set(FULL_VERSION "${BASE_VERSION}.${BUILD_NUMBER}")

file(WRITE "${FIRMWARE_OTA_OUT}"
    "/** @file firmware_ota_build.h @brief 构建期生成的 OTA 单调版本。 */\n"
    "#pragma once\n\n"
    "#include <stdint.h>\n\n"
    "#define FIRMWARE_OTA_BUILD_VERSION UINT64_C(${BUILD_NUMBER})\n")

# 兼容当前 dm.ps1 ota 发布脚本的版本发现方式；后续发布器迁移不影响设备侧协议。
file(WRITE "${DESKMATE_VERSION_OUT}"
    "/** @file deskmate_version.h @brief 构建期生成的 DeskMate 诊断版本。 */\n"
    "#pragma once\n\n"
    "#define DESKMATE_BUILD_VERSION \"${FULL_VERSION}\"\n")

message(STATUS "DeskMate OTA 构建身份: ${FULL_VERSION}, ota_version=${BUILD_NUMBER}")
