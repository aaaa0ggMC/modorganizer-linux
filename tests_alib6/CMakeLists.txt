# WP4 独立验证子工程：alib6 (aaaa0ggmcLib gen6) 能否被 CMake 干净接入
#
#   构建：
#     cmake -S tests_alib6 -B /tmp/mol-alib6-build -G Ninja
#     cmake --build /tmp/mol-alib6-build -j8
#     /tmp/mol-alib6-build/smoke
#
# 该子工程完全独立，不加入根 CMakeLists.txt 的 glob。

cmake_minimum_required(VERSION 3.28)

# alib6 的接口/实现单元里大量使用 `import std;`，需要在 project() 之前打开
# CMake 4.x 的实验开关（激活值随 CMake 版本变化，4.4 对应的值如下）。
# cmake/alib6.cmake 会检查是否真的生效，未生效时给出明确 FATAL_ERROR。
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD f35a9ac6-8463-4d38-8eec-5d6008153e7d)

project(tests_alib6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# smoke.cpp 里用了 `import std;`（需要上面的实验开关已打开）
set(CMAKE_CXX_MODULE_STD ON)

# 由 ../cmake/alib6.cmake 创建 STATIC 目标 mol_alib6（幂等函数 mol_alib6()）
include(../cmake/alib6.cmake)

add_executable(smoke smoke.cpp)
target_link_libraries(smoke PRIVATE mol_alib6)
