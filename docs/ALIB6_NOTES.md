# WP4：alib6（aaaa0ggmcLib gen6）以 CMake 目标接入的可行性结论

日期：2026-10-04
环境：GCC 16.2.1、CMake 4.4.3、Ninja 1.13.2、Arch Linux x86_64
源码：`~/Projs/aaaa0ggmcLib`（只读，未做任何修改）

## 1. 结论

**可以，而且相当干净。** alib6 能被 CMake 以 STATIC 目标 `mol_alib6` 完整构建，
下游 `import alib6;` 的 TU 编译、链接、运行全部正常，冒烟测试 16/16 项通过，退出码 0。

- 接入层：`cmake/alib6.cmake`（函数 `mol_alib6()`，幂等，include 即得目标）
- 验证子工程：`tests_alib6/`（独立 CMakeLists，不加入根 CMake 的 glob）
- 构建目录：`/tmp/mol-alib6-build`（不在仓库内）

```bash
cmake -S tests_alib6 -B /tmp/mol-alib6-build -G Ninja
cmake --build /tmp/mol-alib6-build -j8
/tmp/mol-alib6-build/smoke
```

扫描结果：接口单元 `include/alib6/**/*.cppm` 75 个，实现单元
`modules/alib6/**/*.cpp` 16 个，共 199 个 ninja 步骤（含 std 模块的扫描/编译）。

## 2. 编译耗时（实测）

| 场景 | 耗时 |
| :--- | :--- |
| 冷构建（全新 build 目录） | **25.8 s**（`real`，`-j8`） |
| 只 touch `smoke.cpp` 后增量 | **3.2 s** |
| 仅 configure | < 1 s |

说明：以上为**默认 BuildType（空，无 `-O`）**下的数字，`user` 1m47s / `sys` 13.9s；
若接 `-O2/-O3` 会显著变慢（xmake 侧是 `-O3`），本项目接入时建议沿用根工程的
构建类型配置，并让 `mol_alib6` 只在真正用到它的目标上被链接。

## 3. 踩到的坑

1. **`import std;` 需要 CMake 的实验开关，且必须在 `project()` 之前设。**
   alib6 的接口/实现单元里大量 `import std;`，CMake 侧需要：
   ```cmake
   set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD f35a9ac6-8463-4d38-8eec-5d6008153e7d)  # project() 之前
   project(x LANGUAGES CXX)
   set(CMAKE_CXX_MODULE_STD ON)          # 或目标属性 CXX_MODULE_STD ON
   ```
   激活值（UUID）随 CMake 版本变化，本机 CMake 4.4 对应的值可用
   `strings /usr/bin/cmake | grep -B2 CXX_IMPORT_STD` 附近找到。依赖
   `/usr/lib/libstdc++.modules.json`（GCC 16 自带，`g++ -print-file-name=libstdc++.modules.json`
   可见）。CMake 会自动编译 `bits/std.cc` / `bits/std.compat.cc` 为内置目标
   `@cmake_cxx_std`，无需自己造 std 模块。`cmake/alib6.cmake` 会检查是否生效并给出
   明确 FATAL_ERROR。
2. **实现单元 `.cpp`（`module alib6.xxx;`）不能放进 `FILE_SET CXX_MODULES`。**
   放进去 CMake 直接报错：`Output ... is of type 'CXX_MODULES' but does not provide
   a module interface unit or partition`。正确做法：`.cppm` 进文件集，`.cpp` 作为普通
   `target_sources(... PRIVATE)`；模块扫描（`-fmodules-ts -fdeps-file=... -fdeps-format=p1689r5`）
   + dyndep 会自动推导出"先 BMI 后实现"的顺序，无需手写依赖。
3. **`libstdc++exp` 只有 `.a` 且在 GCC 私有目录里。**
   `/usr/lib/gcc/x86_64-pc-linux-gnu/16/libstdc++exp.a`，`find_library` 默认搜索路径
   找不到它；直接用名字 `-lstdc++exp` 让 gcc 驱动解析可以，`g++ -print-file-name=libstdc++exp.a`
   兜底拿绝对路径也可以（本仓库 CMake 用后者）。
4. **`-I${ALIB6}/include` 是硬需求。** 每个 `.cppm/.cpp` 的全局模块片段都有
   `#include <alib6/config.h>`，扫描阶段和编译阶段都要这条 include path。
5. **`-freflection` 必须 PUBLIC 传播。** 消费方 TU 也要带 `-freflection`（`<meta>` 的
   展开/符号一致性），否则 `import alib6` 的下游编译会出问题。
6. **不要把 `.cppm` 同时列进普通源文件和文件集**（会触发 CMP0211 policy 警告）。
7. **文档与真实 API 有出入，一律以头文件为准**（详见第 5 节）。

## 4. 本项目推荐用法（已全部在 `tests_alib6/smoke.cpp` 验证）

### 4.1 import 写法
```cpp
import std;        // 可选；本 smoke 用纯 modules 写法（不再 #include 标准库）
import alib6;      // 一次性聚合：core/ecs/log/perf/table/data/algo/co/clock/request
```
也可按需 `import alib6.core:cmd;` / `import alib6.data:json;`（分区导入）以减小依赖面。
smoke 只验证了"纯 import、不混 `#include`"这一种写法。

### 4.2 PMR：`mem` 参数怎么传
alib6 全链路接受 `std::pmr::memory_resource* mem = get_default_resource()`，多数是在**最后一个参数**：
```cpp
std::pmr::monotonic_buffer_resource pool;
Logger logger(LoggerConfig{.consumer_count = 0}, &pool);  // (cfg, mem)
AData  doc(&pool);                                        // ctor 直接接 mem
doc.load_from_memory(text);                               // 解析用 doc 构造时绑定的资源
doc.load_from_memory(text, JSON{});                       // 需要换 parser 时显式传 JSON
```
不传即走 `get_default_resource()`；嵌套容器用 `c.get_allocator().resource()` 向下传。

### 4.3 JSON：构造、解析、dump
```cpp
AData doc(&pool);
doc["game"] = "skyrimse";        // 标量直接赋值
doc["nested"]["ok"] = true;      // 递归对象路径自动创建
doc["ports"][0] = 1001;          // 数组自动扩容

bool ok = doc.load_from_memory(R"({"game":"skyrimse"})");   // 默认 parser 就是 JSON
auto s  = doc["game"].to<std::string_view>();
auto n  = doc["loadorder"].to<i64>();   // Value::to<T>()，失败走 ErrorWrapper/异常

std::string out;
alib6::JSON json_engine;                 // JSONConfig 可调 indent / float_precision / 排序
json_engine.dump(out, doc);              // 直接追加进 std::string
// 或 auto js = doc.dump_to_string(json_engine);   // 返回 pmr::string，需 .data()/.size() 转 std::string
```
README/文档里的 `data::dump_json(doc, ...)`、`doc["x"].value().get_str(...)` **已不存在**
（那是 alib5 API）；现在的取值是 `BasicAData::to<T>()` / `Value::to<T>()`，
序列化是 `JSON{}.dump(target, doc)` / `doc.dump_to_string(JSON{})`。

### 4.4 日志初始化
```cpp
Logger logger(LoggerConfig{.consumer_count = 0});   // 0 = 同步直写；N = N 个后台消费线程
logger.append_mod<lot::Console>("console");         // 也可 <lot::File>("file", path)
LogFactory lg(logger, "NetworkWorker");
lg(Severity::Info) << "..." << endlog;              // Severity{Verbose,Debug,Info,Warn,Error,Fatal}
logger.flush();                                     // 冲刷 + 等后台消费线程排空（异步模式尤其需要）
```
`aout` prefab 可零配置直接用：`aout << "..." << endlog;`。文档里的 `LOG_COLOR1(Yellow)`
在 alib6 已删除（alib5 遗留），颜色应通过 `lot::Color` / `ConsoleConfig` 的 color schema 设置。

### 4.5 CLI：声明子命令 / 选项 / 开关
```cpp
Command cmd;                                     // Command(memory_resource* mem = get_default_resource())
cmd.register_option({.name="game", .short_name="-g", .long_name="--game",
                     .description="Target game", .default_val="skyrimse"});
cmd.register_toggle({.name="json", .short_name="-j", .long_name="--json",
                     .description="Emit machine-readable JSON"});
cmd.add_route("status", [](const Command::CommandInput& in) -> Command::CommandOutput {
    // in.has("json")               -> 开关是否出现
    // in.get("game").view()        -> 选项值（std::string_view）
    // in.get<int>("port", 0)       -> 带默认值的强类型取值
    // in.args() / in.arg(0)        -> 剩余位置参数
    // in.key("name")               -> 路由通配符 {name} 捕获值
    return Command::CommandOutput::with_code(0);  // 也可 terminate(code) / no_output()
});
const char* argv[] = {"prog", "status", "--json", "--game", "skyrimse"};
auto outs = cmd.from_args(5, argv);              // argv[0] 会被自动剔除，返回 pmr::vector<CommandOutput>
std::println("{}", cmd.help());                  // 自动生成 ASCII 帮助树
```
`core.md` 文档里的 `cmd.add_option / add_positional / set_handler / CommandContext`
与实际不符；真实 API 是 `register_option / register_toggle / register_options /
register_toggles / add_route / register_handler / from_str / from_args`，
handler 的入参是 `const Command::CommandInput&`（`command_in_t`）。

### 4.6 与本项目（mocore / CLI）的关系
`mol_alib6` 是**独立子工程验证**，没有接入根 `CMakeLists.txt`。后续若在 `cli/` 落地：
`target_link_libraries(cli PRIVATE mol_alib6)` + `cmake_minimum_required(VERSION 3.28)`
+ 在根 `project()` 之前打开 import std 实验开关即可；`-Wall -Wextra` 与之兼容
（本次构建未加告警集，未验证告警是否干净）。

## 5. 构建与 smoke 的真实输出（末段）

`cmake --build /tmp/mol-alib6-build -j8`（rc=0）：
```
[193/199] Building CXX object .../modules/alib6/data/validator.cpp.o
[194/199] Building CXX object .../modules/alib6/data/translator.cpp.o
[195/199] Building CXX object CMakeFiles/smoke.dir/smoke.cpp.o
[196/199] Building CXX object .../modules/alib6/data/toml.cpp.o
[197/199] Linking CXX static library libmol_alib6.a
[198/199] Linking CXX executable smoke
real  0m25.767s   user 1m47.225s   sys 0m13.889s
```

`/tmp/mol-alib6-build/smoke`（exit=0）：
```
[2026-10-04 09:00:46][INFO][smoke][0.02ms]: alib6 smoke: sync logger (consumer_count=0) online
[ PASS ] logger: sync mode (consumer_count=0) + Console target written
[ PASS ] json: JSON::parse 成功解析源字符串
[ PASS ] json: 读取字符串字段 game == skyrimse
[ PASS ] json: 读取整数字段 loadorder == 42
[ PASS ] json: 读取嵌套布尔字段 nested.ok == true
        dump => {
  "game" : "skyrimse",
  "loadorder" : 42,
  "nested" : {
    "ok" : true
  },
  "ports" : [
    1001,
    1002
  ]
}
[ PASS ] json: dump 回字符串包含 game 字段
[ PASS ] json: dump 结果可再次被解析（round-trip）
[ PASS ] cmd: from_args 返回了 dispatch 结果
[ PASS ] cmd: 子命令 status 的 handler 被调用
[ PASS ] cmd: 开关 --json 被识别
[ PASS ] cmd: 选项 --game skyrimse 被解析
[ PASS ] cmd: 无多余位置参数残留
[ PASS ] cmd: handler 返回 CommandOutput code == 0 且 valid
smoke: all checks passed
```

## 6. 未验证的项

- 未接入根 `CMakeLists.txt`（按要求保持根文件不变）；只在 `tests_alib6/` 独立构建。
- 未测 `-O2/-O3`、`RelWithDebInfo`、`Debug`、`shared` 目标形态与对应耗时。
- 未测 aaaa0ggmcLib 的 **alib5** 部分（`include/alib5/*.h` + `modules/alib5/*.cpp`，
  纯头文件实现，接入方式完全不同）。
- 未在 smoke 里覆盖 `aout` prefab、`table`、`ecs`、`perf`、`reflect`（`reflect` 依赖
  `<meta>`，虽然 `-freflection` 已传播，但未实际实例化反射序列化代码）。
- 未测一个 TU 内 `import alib6;` 与 `#include <nlohmann/json.hpp>` 等**文本 include 混用**；
  若 `core/` 继续用 nlohmann_json，需单独验证（不涉及本次范围）。
- 未测 `gtest6`/`test6` 那些 alib6 自带测试能否在 CMake 下跑（需要 gtest）。
- 未测 `MOL_ALIB6_DIR` 指向 `third_party/` 副本时的路径解析（代码逻辑覆盖，但只实测了
  `$HOME/Projs/aaaa0ggmcLib` 这一条默认路径）。
- 未测其他编译器（clang / MSVC）与 GCC 15 及更早版本；`import std` 的实验开关值
  绑定 CMake 4.4，换版本需重新取激活值。
