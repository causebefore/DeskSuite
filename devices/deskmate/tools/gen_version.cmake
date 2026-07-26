# 开发版本号生成脚本
# 被组件 CMakeLists.txt 在 build 阶段通过 custom target(-P) 调用。
# 读取 version.txt 基础版本，拼接构建时间戳，写入版本头文件。
#
# 用法: cmake -DVERSION_FILE=<path> -DOUT=<path> -P gen_version.cmake

file(READ "${VERSION_FILE}" BASE_VERSION)
string(STRIP "${BASE_VERSION}" BASE_VERSION)

string(TIMESTAMP BUILD_TS "%Y%m%d%H%M%S")
set(FULL_VERSION "${BASE_VERSION}.${BUILD_TS}")

file(WRITE "${OUT}"
    "/* 自动生成，请勿手动修改。每次 build 由 CMake custom target 刷新。 */\n"
    "#pragma once\n"
    "#define DESKMATE_BUILD_VERSION \"${FULL_VERSION}\"\n")
message(STATUS "DeskMate OTA 版本号: ${FULL_VERSION}")
