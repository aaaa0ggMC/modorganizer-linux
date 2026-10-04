cmake_minimum_required(VERSION 3.28)
project(mo-linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)

find_package(nlohmann_json REQUIRED)

# ---- core: 纯 C++26，不依赖 Qt ------------------------------------------
file(GLOB CORE_SRC CONFIGURE_DEPENDS core/src/*.cpp)
add_library(mocore STATIC ${CORE_SRC})
target_include_directories(mocore PUBLIC core/include)
target_link_libraries(mocore PUBLIC nlohmann_json::nlohmann_json)

# ---- tests: tests/test_*.cpp 每个文件一个可执行文件 ----------------------
enable_testing()
file(GLOB TEST_SRC CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(src ${TEST_SRC})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE tests)
  target_link_libraries(${name} PRIVATE mocore)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

# ---- shim: Windows API 的 Linux 实现 ---------------------------------------
add_subdirectory(shim)
