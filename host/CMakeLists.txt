# host：用 clang 构建（上游 MSVC 习惯用法 GCC 不接受），Qt 全部隔离在 libmo-game.so，
# 对外只有 C ABI（host/include/mo_game.h）。由根 CMake 以 ExternalProject 调用。
cmake_minimum_required(VERSION 3.28)
project(mo-game LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)          # clang 需要 gnu++ 才打开 -fms-extensions 一类行为
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)   # 不用 C++ modules，省掉 clang-scan-deps
set(CMAKE_AUTOUIC OFF)   # 手动 qt_wrap_ui：上游源码树位于构建目录内，AUTOUIC 会把不同 target 的 .ui 混在一起
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(MOL_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(UIBASE_UP ${MOL_ROOT}/third_party/uibase)
set(GB_UP ${MOL_ROOT}/third_party/game_bethesda/src)
foreach(d ${UIBASE_UP} ${GB_UP})
  if(NOT EXISTS ${d})
    message(FATAL_ERROR "缺少上游源码 ${d}（third_party 未检出）")
  endif()
endforeach()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Qml Quick QuickWidgets)
find_package(spdlog REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)

# ---- shim ---------------------------------------------------------------
set(MOL_SHIM_TESTS OFF)
add_subdirectory(${MOL_ROOT}/shim shim)

# ---- 补丁后的 uibase 副本 + include 别名 ------------------------------------
set(UIBASE_SRC ${CMAKE_BINARY_DIR}/uibase-src)
execute_process(COMMAND ${MOL_ROOT}/scripts/prepare_uibase.sh ${UIBASE_SRC} RESULT_VARIABLE rc)
if(rc)
  message(FATAL_ERROR "prepare_uibase.sh 失败")
endif()
set(ALIAS_DIR ${CMAKE_BINARY_DIR}/alias)
set(UIBASE_INCS ${UIBASE_SRC}/include ${UIBASE_SRC}/include/uibase ${UIBASE_SRC}/include/uibase/game_features)
set(GB_INCS ${GB_UP}/gamebryo ${GB_UP}/creation ${GB_UP}/games/skyrimse)
string(REPLACE ";" ":" _inc_dirs "${MOL_ROOT}/shim/include;${UIBASE_INCS};${GB_INCS}")
execute_process(COMMAND python3 ${MOL_ROOT}/scripts/gen_include_aliases.py ${ALIAS_DIR}
                        "${UIBASE_SRC}/src:${UIBASE_SRC}/include:${GB_UP}" "${_inc_dirs}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE _alias_out)
if(rc)
  message(FATAL_ERROR "gen_include_aliases.py 失败")
endif()
message(STATUS "include 大小写别名: ${_alias_out}")

# 所有上游 TU 共用的编译选项
set(UP_FLAGS -fms-extensions "SHELL:-include winshim_prelude.h" -w)

# ---- uibase（静态库）---------------------------------------------------------
file(GLOB UIBASE_CPP ${UIBASE_SRC}/src/*.cpp)
list(FILTER UIBASE_CPP EXCLUDE REGEX "/(registry|pch)\\.cpp$")   # registry 用无头版替换；pch.cpp 仅用于 MSVC PCH
file(GLOB UIBASE_UI ${UIBASE_SRC}/src/*.ui)
qt_wrap_ui(UIBASE_UI_HDRS ${UIBASE_UI})
file(GLOB_RECURSE UIBASE_HDR ${UIBASE_SRC}/include/uibase/*.h)
add_library(uibase STATIC ${UIBASE_CPP} ${UIBASE_UI_HDRS} ${UIBASE_HDR}
            ${CMAKE_CURRENT_LIST_DIR}/overrides/uibase/registry.cpp)
# uibase 的 include/uibase 目录里有 strings.h，会遮蔽 glibc 的 <strings.h>（<string.h> 内部在 extern "C" 块里包含它）。
# 后果：uibase 自己的 TU 若用 -I，MOBase::ireplace_all 会被截获并编成 C 链接（符号未修饰），
# 而使用方（-idirafter）期望 C++ 链接 → 运行期 undefined symbol。
# 所以对 uibase 本身和消费者都用 -idirafter（角括号），并用 -iquote 保证 uibase 自己的 "strings.h" 仍能解析到本地文件。
target_include_directories(uibase PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${MOL_ROOT}/shim/include ${ALIAS_DIR} ${UIBASE_SRC}/include
                                         PRIVATE ${UIBASE_SRC}/src)
target_compile_options(uibase PUBLIC "SHELL:-idirafter ${UIBASE_SRC}/include/uibase"
                                     "SHELL:-idirafter ${UIBASE_SRC}/include/uibase/game_features"
                                     PRIVATE "SHELL:-iquote ${UIBASE_SRC}/include/uibase"
                                             "SHELL:-iquote ${UIBASE_SRC}/include/uibase/game_features")
target_compile_definitions(uibase PUBLIC SPDLOG_USE_STD_FORMAT PRIVATE UIBASE_EXPORT MOL_SPDLOG_COMPAT)
target_compile_options(uibase PRIVATE ${UP_FLAGS} "SHELL:-include pch.h")
set_source_files_properties(${UIBASE_SRC}/src/pch.h PROPERTIES HEADER_FILE_ONLY ON)
target_link_libraries(uibase PUBLIC winshim Qt6::Widgets Qt6::Network Qt6::QuickWidgets
                             PRIVATE spdlog::spdlog_header_only Qt6::Qml Qt6::Quick)

# ---- game_bethesda：gamebryo + creation + skyrimse（静态库，零修改）-------------
file(GLOB GAMEBRYO_CPP ${GB_UP}/gamebryo/*.cpp)
file(GLOB GAMEBRYO_UI ${GB_UP}/gamebryo/*.ui)
qt_wrap_ui(GAMEBRYO_UI_HDRS ${GAMEBRYO_UI})
file(GLOB GAMEBRYO_HDR ${GB_UP}/gamebryo/*.h)
file(GLOB SKYRIMSE_CPP ${GB_UP}/games/skyrimse/*.cpp)
file(GLOB SKYRIMSE_HDR ${GB_UP}/games/skyrimse/*.h)
add_library(game_bethesda STATIC ${GAMEBRYO_CPP} ${GAMEBRYO_UI_HDRS} ${GAMEBRYO_HDR}
            ${GB_UP}/creation/creationgameplugins.cpp ${GB_UP}/creation/creationgameplugins.h
            ${SKYRIMSE_CPP} ${SKYRIMSE_HDR})
target_include_directories(game_bethesda PUBLIC ${GB_INCS})
target_compile_options(game_bethesda PRIVATE ${UP_FLAGS})
# moc 要能找到 <iplugingame.h> 等（编译用 -idirafter，AUTOMOC 看不到）
set_target_properties(game_bethesda PROPERTIES AUTOMOC_MOC_OPTIONS "-I${UIBASE_SRC}/include/uibase;-I${UIBASE_SRC}/include/uibase/game_features;-I${UIBASE_SRC}/include")
target_link_libraries(game_bethesda PUBLIC uibase ZLIB::ZLIB PkgConfig::LZ4)

# ---- libmo-game.so：宿主（假 IOrganizer + C ABI）-------------------------------
file(GLOB HOST_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
if(HOST_SRC)
  add_library(mo-game SHARED ${HOST_SRC})
  target_include_directories(mo-game PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
  target_compile_options(mo-game PRIVATE -fms-extensions "SHELL:-include winshim_prelude.h")
  target_link_libraries(mo-game PRIVATE game_bethesda)
endif()
