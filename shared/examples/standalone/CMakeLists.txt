# 独立编译验证工程：只依赖 shared/components 下的可发布组件，
# 不引用任何 devices/ 工程，用于验证组件自包含性。
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(standalone_components)
