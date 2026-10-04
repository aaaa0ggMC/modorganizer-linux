# ============================================================================
# alib6 (aaaa0ggmcLib gen6) -> CMake 接入层
#
# 目标：把 aaaa0ggmcLib 的 alib6 部分（C++26 modules 库）以 STATIC 目标
#       `mol_alib6` 的形式接入本项目，替代 nlohmann_json / spdlog / 手写 CLI。
#
# 用法：
#   # 必须在 project() 之前打开 CMake 的 `import std` 实验开关（alib6 的
#   # 接口/实现单元里大量使用 `import std;`）。
#   set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD <gate-uuid>)  # 见本文件下方 import std 检查段
#   project(... LANGUAGES CXX)
#   ...
#   include(cmake/alib6.cmake)   # 提供幂等函数 mol_alib6() 并自动创建目标
#   target_link_libraries(mylib PRIVATE mol_alib6)
#
# 源码定位：缓存变量 MOL_ALIB6_DIR（PATH，可为空）。为空时依次尝试
#   1. ${CMAKE_SOURCE_DIR}/third_party/aaaa0ggmcLib
#   2. 环境变量 $ENV{MOL_ALIB6_DIR}
#   3. $ENV{HOME}/Projs/aaaa0ggmcLib
# 找不到含 include/alib6/main.cppm 的目录时报 FATAL_ERROR。
#
# 布局约定（对应 xmake/alib6.lua）：
#   include/alib6/**/*.cppm  -> 接口单元（FILE_SET CXX_MODULES）
#   modules/alib6/**/*.cpp   -> 实现单元（`module alib6.xxx;`，普通源文件）
#   PUBLIC 传播：-freflection、stdc++exp、cxx_std_26、include 目录
#
# 注意：alib6 源码目录只读，本文件绝不修改它；也不写死任何绝对路径。
# ============================================================================

# ---- CMake 的 `import std` 实验开关 -----------------------------------------
# alib6 的 .cppm/.cpp 里大量 `import std;`，需要 CMake 内置的 std 模块目标。
# 该开关必须在 project() 之前设置才会生效；这里提前检查并给出清晰报错。
if(NOT CMAKE_CXX_COMPILER_IMPORT_STD)
    message(FATAL_ERROR
        "alib6: 需要 CMake 的 `import std` 支持，但当前未开启。\n"
        "请在 project() 之前设置实验开关（CMake 4.x 的激活值可用 "
        "`cmake --help-experimental` 或 strings /usr/bin/cmake | grep CXX_IMPORT_STD 附近找到）：\n"
        "    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD <gate-uuid>)\n"
        "当前 CMake 未发现 libstdc++ 模块元数据（CMAKE_CXX_COMPILER_IMPORT_STD_ERROR_MESSAGE="
        "${CMAKE_CXX_COMPILER_IMPORT_STD_ERROR_MESSAGE}）。")
endif()

# ---- 源码目录缓存变量 -------------------------------------------------------
set(MOL_ALIB6_DIR "" CACHE PATH
    "aaaa0ggmcLib (alib6) 源码根目录，需包含 include/alib6/main.cppm 与 modules/alib6/**")

# 在候选路径中定位 alib6 源码根目录，结果写入父作用域的 _mol_alib6_root。
function(_mol_alib6_locate_root)
    set(_mol_candidates "")

    if(MOL_ALIB6_DIR)
        list(APPEND _mol_candidates "${MOL_ALIB6_DIR}")
    endif()

    list(APPEND _mol_candidates
        "${CMAKE_SOURCE_DIR}/third_party/aaaa0ggmcLib"
        "$ENV{MOL_ALIB6_DIR}"
        "$ENV{HOME}/Projs/aaaa0ggmcLib"
    )

    foreach(_dir IN LISTS _mol_candidates)
        if(_dir AND EXISTS "${_dir}/include/alib6/main.cppm")
            set(_mol_alib6_root "${_dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    message(FATAL_ERROR
        "alib6: 找不到源码目录（需要其中存在 include/alib6/main.cppm）。\n"
        "已尝试（按顺序）：\n"
        "  1. MOL_ALIB6_DIR(cache)     = '${MOL_ALIB6_DIR}'\n"
        "  2. <src>/third_party/aaaa0ggmcLib = '${CMAKE_SOURCE_DIR}/third_party/aaaa0ggmcLib'\n"
        "  3. 环境变量 MOL_ALIB6_DIR   = '$ENV{MOL_ALIB6_DIR}'\n"
        "  4. HOME/Projs/aaaa0ggmcLib  = '$ENV{HOME}/Projs/aaaa0ggmcLib'\n"
        "请通过 -DMOL_ALIB6_DIR=/path/to/aaaa0ggmcLib 或环境变量 MOL_ALIB6_DIR 指定。")
endfunction()

# ---- 创建 STATIC 目标 mol_alib6 --------------------------------------------
# 幂等：重复调用只会提示一次。
function(mol_alib6)
    if(TARGET mol_alib6)
        message(STATUS "alib6: 目标 mol_alib6 已存在，跳过重复创建")
        return()
    endif()

    _mol_alib6_locate_root()
    set(MOL_ALIB6_DIR "${_mol_alib6_root}" CACHE PATH
        "aaaa0ggmcLib (alib6) 源码根目录，需包含 include/alib6/main.cppm 与 modules/alib6/**"
        FORCE)
    message(STATUS "alib6: 源码目录 = ${MOL_ALIB6_DIR}")

    # ---- 第三方依赖：glm / rapidjson / toml++（系统已装，仅头文件） ---------
    find_path(MOL_ALIB6_GLM_INCLUDE_DIR NAMES glm/glm.hpp)
    find_path(MOL_ALIB6_RAPIDJSON_INCLUDE_DIR NAMES rapidjson/document.h)
    find_path(MOL_ALIB6_TOMLPLUSPLUS_INCLUDE_DIR NAMES toml++/toml.hpp)

    foreach(_pair "GLM;glm/glm.hpp" "RAPIDJSON;rapidjson/document.h" "TOMLPLUSPLUS;toml++/toml.hpp")
        list(GET _pair 0 _name)
        list(GET _pair 1 _hdr)
        if(NOT MOL_ALIB6_${_name}_INCLUDE_DIR)
            message(FATAL_ERROR
                "alib6: 未找到依赖头文件 <${_hdr}>，请安装对应系统包（glm / rapidjson / toml++）。")
        endif()
    endforeach()

    # stdc++exp：reflection 运行库。注意它位于 GCC 自己的私有库目录
    # （如 /usr/lib/gcc/x86_64-pc-linux-gnu/16），find_library 默认搜不到，
    # 用编译器驱动自带的 -print-file-name 兜底。
    find_library(MOL_ALIB6_STDCXXEXP_LIBRARY NAMES stdc++exp)
    if(NOT MOL_ALIB6_STDCXXEXP_LIBRARY)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++exp.a
            OUTPUT_VARIABLE _mol_stdcxxexp_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(EXISTS "${_mol_stdcxxexp_path}")
            set(MOL_ALIB6_STDCXXEXP_LIBRARY "${_mol_stdcxxexp_path}")
        endif()
    endif()
    if(NOT MOL_ALIB6_STDCXXEXP_LIBRARY)
        message(FATAL_ERROR
            "alib6: 未找到 libstdc++exp（GCC reflection 支持库），当前 GCC 不支持 -freflection？")
    endif()

    # ---- 源文件收集（只读扫描 alib6 源码目录） ------------------------------
    file(GLOB_RECURSE _mol_cppm_files CONFIGURE_DEPENDS
        "${MOL_ALIB6_DIR}/include/alib6/*.cppm")
    file(GLOB_RECURSE _mol_impl_files CONFIGURE_DEPENDS
        "${MOL_ALIB6_DIR}/modules/alib6/*.cpp")

    list(LENGTH _mol_cppm_files _mol_cppm_count)
    list(LENGTH _mol_impl_files _mol_impl_count)
    if(_mol_cppm_count EQUAL 0 OR _mol_impl_count EQUAL 0)
        message(FATAL_ERROR
            "alib6: 在 ${MOL_ALIB6_DIR} 下没有扫描到模块文件"
            "（接口 ${_mol_cppm_count} 个 / 实现 ${_mol_impl_count} 个）。")
    endif()
    message(STATUS "alib6: 接口单元 ${_mol_cppm_count} 个，实现单元 ${_mol_impl_count} 个")

    add_library(mol_alib6 STATIC)

    # 接口单元：C++ modules 文件集（CMake 会自动处理 BMI 顺序与 dyndep）
    target_sources(mol_alib6
        PUBLIC
            FILE_SET alib6_cxx_modules
            TYPE CXX_MODULES
            BASE_DIRS
                "${MOL_ALIB6_DIR}/include"
                "${MOL_ALIB6_DIR}/modules"
            FILES ${_mol_cppm_files}
    )

    # 实现单元：`module alib6.core;` 这类模块实现文件，作为普通源文件编入；
    # 与接口 BMI 的先后顺序由 CMake 的模块扫描 + dyndep 自动推导。
    target_sources(mol_alib6 PRIVATE ${_mol_impl_files})

    # ---- 使用要求 -----------------------------------------------------------
    target_include_directories(mol_alib6 PUBLIC
        "${MOL_ALIB6_DIR}/include"
        "${MOL_ALIB6_GLM_INCLUDE_DIR}"
        "${MOL_ALIB6_RAPIDJSON_INCLUDE_DIR}"
        "${MOL_ALIB6_TOMLPLUSPLUS_INCLUDE_DIR}"
    )
    target_compile_features(mol_alib6 PUBLIC cxx_std_26)
    target_compile_options(mol_alib6 PUBLIC -freflection)
    target_link_libraries(mol_alib6 PUBLIC "${MOL_ALIB6_STDCXXEXP_LIBRARY}")

    # alib6 源码里用了 `import std;`，需要 std 模块目标
    set_target_properties(mol_alib6 PROPERTIES CXX_MODULE_STD ON)
endfunction()

# include(cmake/alib6.cmake) 即得到目标；也可在外部显式调用 mol_alib6()（幂等）。
mol_alib6()
