cmake_minimum_required(VERSION 3.28)

# alib6 大量使用 `import std;`：CMake 4.x 需要在 project() 之前打开实验开关（4.4 对应的值）。
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD f35a9ac6-8463-4d38-8eec-5d6008153e7d)

project(mo-linux LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- alib6（aaaa0ggmcLib）：日志 / JSON(AData+反射) / 命令行 ---------------------
include(cmake/alib6.cmake)
mol_alib6()

# ---- core: PMR 风格的纯 C++26 逻辑（无 Qt）----------------------------------
file(GLOB CORE_SRC CONFIGURE_DEPENDS core/src/*.cpp)
add_library(mocore STATIC ${CORE_SRC})
target_include_directories(mocore PUBLIC core/include)
target_compile_options(mocore PRIVATE -Wall -Wextra)
find_package(CURL REQUIRED)
find_package(EXPAT REQUIRED)
find_package(ZLIB REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
target_link_libraries(mocore PUBLIC mol_alib6 CURL::libcurl EXPAT::EXPAT ZLIB::ZLIB PkgConfig::LZ4 yaml-cpp::yaml-cpp)

# ---- CLI --------------------------------------------------------------------
file(GLOB CLI_SRC CONFIGURE_DEPENDS cli/*.cpp)
if(CLI_SRC)
  add_executable(mo-linux ${CLI_SRC})
  target_compile_options(mo-linux PRIVATE -Wall -Wextra)
  target_link_libraries(mo-linux PRIVATE mocore mol_alib6 ${CMAKE_DL_LIBS})
  # `mo-linux docs`：cli/cmd_docs.cpp 用 #embed 把这些文档编进二进制
  set_source_files_properties(cli/cmd_docs.cpp PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/GUIDE.md;${CMAKE_CURRENT_SOURCE_DIR}/docs/AGENT.md;${CMAKE_CURRENT_SOURCE_DIR}/docs/CLI.md;${CMAKE_CURRENT_SOURCE_DIR}/HANDBOOK.md;${CMAKE_CURRENT_SOURCE_DIR}/README.md")
endif()

# ---- libmol-cow.so：工具运行时的写时复制（LD_PRELOAD 注入 Wine 进程，见 cow/mol_cow.c）----------
# 纯 C、只用老 glibc 也有的符号（dlsym 走 libdl），装在 mo-linux 旁边。
enable_language(C)
add_library(mol-cow SHARED cow/mol_cow.c)
set_target_properties(mol-cow PROPERTIES C_STANDARD 11 C_VISIBILITY_PRESET hidden PREFIX "lib" OUTPUT_NAME "mol-cow")
target_compile_options(mol-cow PRIVATE -Wall -Wextra -O2 -fno-strict-aliasing -U_FORTIFY_SOURCE)
target_link_libraries(mol-cow PRIVATE ${CMAKE_DL_LIBS})

# 测试用的假游戏层（tests/check_cli.py 用 MOL_GAME_LIB 指向它，测依赖游戏版本的逻辑）
add_library(fake-mo-game SHARED tests/fake_game_host.c)
set_target_properties(fake-mo-game PROPERTIES C_STANDARD 11 C_VISIBILITY_PRESET hidden)

# ---- tests: tests/test_*.cpp 每个文件一个可执行文件 ----------------------------
enable_testing()
file(GLOB TEST_SRC CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(src ${TEST_SRC})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE tests)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  target_link_libraries(${name} PRIVATE mocore)
  if(name MATCHES "^test_cli_")
    target_link_libraries(${name} PRIVATE mol_alib6)   # CLI 层的测试会 import alib6
  endif()
  add_test(NAME ${name} COMMAND ${name})
endforeach()
if(TARGET test_cow)
  add_dependencies(test_cow mol-cow)
  target_compile_definitions(test_cow PRIVATE MOL_COW_LIB_PATH="$<TARGET_FILE:mol-cow>")
endif()

# ---- shim 的单元测试（Windows API 的 Linux 实现；host 子工程里也会再编一份给 clang 用）-----
add_subdirectory(shim)

# ---- host：libmo-game.so（clang 子工程；Qt + uibase + 上游 game_bethesda）-----------
option(MOL_BUILD_HOST "构建 libmo-game.so（需要 clang、Qt6、spdlog、lz4、zlib 与 third_party 源码）" ON)
if(MOL_BUILD_HOST AND EXISTS ${CMAKE_SOURCE_DIR}/third_party/uibase)
  include(ExternalProject)
  ExternalProject_Add(mo_game_host
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/host
    BINARY_DIR ${CMAKE_BINARY_DIR}/host
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    USES_TERMINAL_BUILD ON)
endif()
