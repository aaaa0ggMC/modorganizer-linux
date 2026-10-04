# mo-linux HANDBOOK

> 面向**审阅与复核**：设计意图、关键取舍、当前进度、未验证项、可执行的检查清单。
> 状态快照：2026-10-04（**接续点**，见 §0）。凡标 **[已验证]** 的都有可重跑的命令；**[未验证]** 的没有，别当成事实。

## 0. 接续点摘要（从这里开始读）

**一句话**：Linux 上已经能用**零修改的上游 MO2 游戏插件**读取真实 Skyrim SE，并把「游戏 + mod」合并成符号链接农场，Proton 容器内实测能穿过链接读到游戏文件；CLI 外壳已接续完成并完整复核，包含 `game info`；真正启动游戏、plugins.txt 接线等还没做。

**已完成并合入 `main`（均有可重跑的测试，见 §13）**
- core：casefold 合并、链接农场、MO2 格式、实例模型、runner、`GameHost`（dlopen）。manifest 与 `mo-linux.json` 已迁到 alib6 `AData`，**core 里不再有 nlohmann**。
- shim（libwinshim）：路径/errno/大小写解析、文件/映射/PE 头、时间、shell（含 `SHFileOperationW` 的 DELETE/COPY/MOVE/RENAME）、COM 存根、ini、PE 版本资源、Wine 注册表（只读）。5 组测试在 ASan/UBSan 下通过。
- host（libmo-game.so，clang）：上游 `game_bethesda`（gamebryo+creation+skyrimse）**零修改**编译链接；假 `IOrganizer`；C ABI：`create / info_json / set_profile / mappings_json / initialize_profile / about_to_run`。
- **在本机真实 Skyrim SE 上实测**：`info_json` 返回正确的目录、DLC、可执行文件、SKSE、ini 名、`gameVersion`（见 §7.5）；`mappings()` 返回 profile 的 `plugins.txt/loadorder.txt` → 游戏 AppData 的映射；`initializeProfile` 复制出正确内容；`prepareIni` 正确读写 ini。
- 农场在真实数据上：176 文件，合并 ≤10ms、应用约 2ms、二次 plan 为 0。Proton 9.0(Beta) + 一次性前缀：容器能列出农场，并通过链接复制出与原文件逐字节一致的 249MB `Skyrim.esm`。

**WP8 已接续完成（当前主仓库；旧 `../modorganizer-linux-wp8` 留存）**
- 复用原 OpenCode WP8 session，由 Step Plan `step-5-preview#low` 删除手写选项解析，加入 `CommandInput` 适配器与公共声明表、位置参数数量校验。最终编译与行为修复由 Codex 独立完成。
- `game info` 使用 core 的 `GameHost` 只读加载上游游戏层；缺失/无效库返回 `game_unavailable`。真实 host + 假游戏目录、假前缀的 CLI 集成检查已通过，不创建前缀、不启动游戏。
- stderr 使用 alib6 Logger，默认 warn+；`-q` 静默（含用法错误）；stdout JSON envelope 与日志分流。事件 JSON 也使用 alib6；修复 fd 解析悬垂指针与整数溢出、FIFO 测试空跑、百分比阈值取整。
- `apply` 空树首次创建 marker 时 `changed:true`；失败发送 `done:false`。超过 256 个操作的分批 apply、manifest 完整性、幂等与 unlink 不删除游戏源文件均已检查。
- 完整构建、ctest 14/14、CLI e2e 125/125 与 `tests/check_cli.py` 22/22 均通过。Unix socket 事件测试需要允许本地 bind；受限沙箱内会被拒绝，应在允许本地 socket 的环境运行，不能跳过后算通过。

**对 alib6（`~/Projs/aaaa0ggmcLib`）的修改：工作区未提交，需作者自行提交**（5 个文件，见 §6.5）。

**当前明确未做项**：真正启动 `SkyrimSE.exe`（需用户确认）、`plugins sync` 与 `mappings()` 的符号链接物化（R9）、overwrite 捕获、`doctor`、固定 `third_party` 提交、LICENSE。

## 1. 目标与边界

做一个 **Linux 原生的 Mod Organizer 2 启动器 CLI**，首个目标游戏是 **Skyrim Special Edition**：

1. 读取 MO2 的磁盘格式（实例目录、`profiles/<name>/modlist.txt`、`plugins.txt`、`ModOrganizer.ini`），可与真 MO2（在 Wine 里）共用同一份数据。
2. 把「游戏本体 + 启用的 mod + overwrite」合并成一个**符号链接农场**（虚拟的游戏根目录），Steam 原目录永不被修改。
3. 用 **Proton/Wine** 从农场里启动游戏或 SKSE。
4. CLI **无状态、幂等、`--json`**；GUI（另行开发）只解析 CLI 的 stdout / 事件流。

**不做（本阶段）**：不移植 MO2 的 Qt GUI；不实现 usvfs；不做 mod 安装（解压/FOMOD）；不做 Nexus 登录/下载（见 §12）；不支持 Skyrim SE 以外的游戏（但游戏层是上游代码，扩展成本低）。

## 2. 架构

```
你的 GUI（任意技术栈）
   │  只依赖：stdout JSON envelope、退出码、--events 的 NDJSON
   ▼
mo-linux (CLI, GCC, C++26, alib6)            ← cli/
   │ 调用
   ├─ mocore (纯 C++26, PMR, 无 Qt)           ← core/   merge / linkfarm / mo2fmt / instance / runner / game_host
   └─ dlopen ─► libmo-game.so (clang, Qt)     ← host/   C ABI；内部：假 IOrganizer + 上游游戏插件
                    │ 链接
                    ├─ 上游 game_bethesda（gamebryo+creation+skyrimse，**一字未改**） ← third_party/
                    ├─ 上游 uibase（构建时打补丁的副本）                              ← third_party/ + patches/
                    └─ libwinshim.so（Windows API 的 Linux 实现）                    ← shim/
```

**分层理由**：Qt、MSVC 习惯用法、Windows API 全部被关在 `libmo-game.so` 里；CLI 与 core 是干净的 C++26/PMR，GUI 完全看不到这些依赖。两个编译器的产物只通过 **C ABI（JSON 字符串）** 交互，规避 clang/GCC 的 ABI 与模块差异。

## 3. 关键设计决策（含被否决的方案）

| # | 决策 | 理由 | 被否决的备选 |
|---|---|---|---|
| D1 | **不 fork MO2 GUI，只复用数据格式与游戏层** | MO2 核心是 usvfs（hook WinAPI），移植量远大于重写 CLI | fork 整个 MO2 |
| D2 | **符号链接农场**，农场是独占的"启动镜像目录" | 不需 root；Steam 原目录不被改；幂等好做；大小写合并是确定性算法 | overlayfs（大小写不合并、要 userns 实测）、复制（占空间） |
| D3 | **大小写合并由我们自己做**（trie + ASCII casefold） | Wine 只在"查找"时不敏感，不会把 `Textures/` 与 `textures/` 合并 | 依赖 Wine |
| D4 | **上游 game_bethesda 零修改**：shim 头文件 + libwinshim + include 大小写别名 | 上游修 bug / 增游戏时直接受益 | 手抄游戏规则 |
| D5 | **uibase 用补丁副本**（`patches/uibase/`），而非零修改 | `QUuid(GUID)`、`QImage::fromHICON` 等 Windows 专属代码 shim 补不了；补丁只 1 个文件 2 处 | 重写 uibase |
| D6 | **host 用 clang 单独构建**，对外只有 C ABI | GCC 不接受上游的类作用域显式特化、`virtual ~X() = 0 {}` 等 MSVC 习惯用法 | 全用 GCC + 更多补丁 |
| D7 | **全面 PMR**（`mol::string`/`mol::vector` + 末参数 `mr* mem`） | 用户预期小对象极多；便于 arena 一次释放 | 普通 STL |
| D8 | **用自己的库 alib6**（`AData`+反射 JSON、`Command`、`Logger`），core/CLI 内不再混用 nlohmann | 用户要求；已验证反射可序列化嵌套 PMR 结构体；core 的 manifest 与 `mo-linux.json` 已迁到 `AData` | nlohmann/spdlog/CLI11 |
| D9 | 进度走 `--events fd:N\|fifo:P\|unix:P`（NDJSON） | GUI 作父进程时 fd 最简单；其余两种覆盖非父子场景；与最终 envelope 分流 | 把进度混进 stdout |
| D10 | 每个下游工作包用独立 git worktree，review 后合并 | 互不破坏构建；可回滚 | 共用工作区 |

## 4. 目录与模块

| 路径 | 职责 | 状态 |
|---|---|---|
| `core/include/mol/pmr.hpp` | PMR 约定与别名（`mol::string/vector/mr`） | 完成 |
| `core/.../casefold` `merge` | ASCII casefold；`scan_layer` + `merge_listings`（trie interning，确定性输出） | 完成，测试通过 |
| `core/.../linkfarm` | `plan_farm / apply_farm / remove_farm`；marker+manifest；幂等；拒绝触碰外来内容 | 完成，14 个测试 |
| `core/.../mo2fmt` | modlist/plugins/loadorder/ini/`wine_to_unix` | 完成；**缺 Ini/loadorder 的 writer** |
| `core/.../instance` | 读实例、`init`、mods 启停/移动、`build_farm_model`、`plan/apply_instance` | 完成，6 个测试 |
| `core/.../runner` | `build_launch`（Proton/Wine argv+env）、`spawn_launch` | 完成，纯函数部分有测试；**未真实启动过** |
| `core/.../game_host` | dlopen 封装 `libmo-game.so` | 完成；真实加载已用 C 探针验证，`GameHost` 已经由 `tests/check_cli.py` 通过 CLI 加载真实 .so + 假目录验证（`test_game_host` 仍只测错误路径） |
| `cli/` | 命令路由、envelope、events、各子命令 | 完成（`CommandInput` / Logger / events / `game info`，见 §0、§10） |
| `shim/include` | Windows 头文件 shim（`windows.h` 等；声明即契约） | 完成 |
| `shim/src` | `internal`、`files`、`misc`、`ini`、`version`、`registry` | 完成（5 组测试，ASan/UBSan 通过） |
| `host/` | clang 子工程：uibase 副本 + game_bethesda + 假 IOrganizer + C ABI | 编译链接、运行均已在真实 Skyrim 上验证（部分接口，见 R1） |
| `patches/uibase/` | 对 uibase 的补丁（保留 CRLF） | 1 个补丁 |
| `scripts/` | `gen_include_aliases.py`、`prepare_uibase.sh`、`make_uibase_patch.py`、`syntax_check.sh` | 完成 |
| `cmake/alib6.cmake` `tests_alib6/` | alib6 的 CMake 接入与冒烟 | 完成（16/16） |
| `third_party/` | `uibase`、`game_bethesda`（git 忽略，需自行检出，见 §8） | — |

## 5. 核心规则（规范性）

### 5.1 合并（casefold merge）
1. 路径每一级用 **ASCII 小写**作 key；非 ASCII 字节原样（Skyrim 资源路径实际为 ASCII）。
2. 规范名 = **最先引入**该 key 的层的原始大小写；层 0（游戏本体）最先，所以游戏自带的 `Data/Textures` 等写法被保留。
3. 同 key 文件：高层覆盖低层，产生 `Conflict{winner, losers}`。
4. 同 key 目录/文件撞名：高层决定类型，被否决者丢弃（含子树）并产生 `Warning`。
5. 隐式父目录自动生成；输出按路径字节序排序；内部不依赖遍历顺序 → **确定性**。
- 已用对抗用例复核 **[已验证]**：同层内 `Data/textures/y.dds`、`DATA/Textures/x.dds`、`data/TEXTURES/z.dds` → 合并成 `Data/Textures/` 且三个叶子全在，游戏的大小写保留。

### 5.2 农场层序（`build_farm_model`）
层 0 = 游戏本体（prefix 空）→ profile 中**启用且目录存在**的 mod（低→高，prefix `Data`，分隔符跳过）→ overwrite（prefix `Data`，目录存在才有）。modlist 文件首行为**最高**优先级，API 统一返回低→高。mod 目录缺失 → 警告并跳过。

### 5.3 链接农场
- 农场根含 `.mol-farm.json`（marker + manifest：`{"version":1,"created":[…]}`）。根非空且无 marker → 拒绝（`farm_not_owned`）。
- `plan` = diff(期望, 实际+manifest)；顺序：先清理（路径逆序），再创建（正序）。`apply` 后再 `plan` **必为空**（幂等）。
- 不跟随符号链接递归；删除一律非递归，遇到用户内容宁可保留也不 `remove_all`；中途失败也会把已完成部分落盘。

## 6. 「零修改上游」机制与偏离清单

**对 game_bethesda：零修改** **[已验证：`git -C third_party/game_bethesda status` 干净；gamebryo+creation+skyrimse 编译链接通过]**。手段：
1. `shim/include/windows.h` 等：在 include 路径最前面提供 Windows 头；`WCHAR=wchar_t`（Linux 为 4 字节，**不可**用 `-fshort-wchar`）。
2. `scripts/gen_include_aliases.py`：上游有大小写不一致的 `#include`（如 `skyrimSEdataarchives.h`），自动生成符号链接别名目录。
3. `winshim_prelude.h`（`-include`）：抹平 `__declspec/__stdcall` 等。
4. include 顺序陷阱：uibase 的 `include/uibase/strings.h` 会遮蔽 glibc `<strings.h>`（`<string.h>` 内部包含它）。消费者必须用 `-idirafter` 引入该目录，uibase 自己的 TU 才用 `-I`。
5. 编译器用 **clang**。

**对 uibase：有偏离（均在 `patches/uibase/`，可复现）**：
- `utility.cpp`：`QUuid(id).toString()`→`what`（`QUuid(GUID)` 仅 Windows）；`iconForExecutable` 返回通用图标（`QImage::fromHICON` 仅 Windows）。
- `registry.cpp` 整体替换为 `host/overrides/uibase/registry.cpp`（无头版，不弹对话框）。
- 漏洞提醒：补丁与上游版本绑定；升级 uibase 必须重跑 `scripts/make_uibase_patch.py` 并复核。

## 6.5 对 alib6 的改动（在 `~/Projs/aaaa0ggmcLib`，**工作区未提交**，需作者自行提交）

按用户授权修复了 alib6 的 4 处缺陷。涉及文件：`include/alib6/core/cmd.cppm`、`modules/alib6/core/cmd.cpp`、`modules/alib6/data/json.cpp`、`tests/alib6/test_cmd.cpp`、`tests/alib6/test_adata.cpp`。回归：alib6 的 cmd/parser/router/adata 测试共 **35 个全部通过**（用 `scratchpad` 里的 CMake 工程 + gtest 跑，因为 alib6 自己用 xmake；改动前 cmd/parser/router 为 12 个全过）。

1. **前置选项**：第一个路由 token 之前的选项/开关（`prog -j -i /x mods list`）此前不会让路由下降，整条路由被挤成位置参数。现在 `dispatch_pipeline` 在 `router.match` 前做"根层预扫描"，选项落入 main 层，`has/get` 跨层可见。
2. **`--` 终止符**：此前 `--` 后的 `-j` 仍被当作开关，且 `--` 进入 `args()`。现在 `--` 之后一律是位置参数，`--` 本身被吞掉（含前置位置）。
3. **`Option::name` 吞路由 token**：`.name="instance"` 会把同名子命令 token 当选项吞掉（WP8 因此花了很久排查）。现在 `name` 只在没有 `long_name/short_name` 时才参与 token 匹配（`token_keys`）；只声明 `name` 的选项行为不变（有回归用例）。**注意这是行为变更**：此前同时声明了破折号别名和 `name` 的用法，裸 `name` 不再能匹配 token。
4. **JSON 转义不合法**：`dump` 复用了 C 风格的 `str::escape`（输出 `\a \v \e \xNN`，非合法 JSON），对象键完全不转义，`ensure_ascii` 时非 BMP 用 `\UXXXXXXXX`（非法）。现在 `json.cpp` 有独立的 RFC 8259 转义（控制字符 `\u00XX`、键转义、代理对、非法 UTF-8→U+FFFD），`str::escape` 本身未改。

- 实验结论（可复核，脚本在 scratchpad）：选项在第一个路由 token 之后放哪里都行；`--name=value` 与 `--name value` 均支持；`has()` 扫描所有层，`get()` 从当前层往上回溯。
- **已核实**：当前 alib6 支持命令前、命令后以及位置参数后的选项；WP8 已删除 `cli/args.cpp` 的选项自解析，并用真实 CommandInput 单测与 CLI e2e 验证。`--` 后的 token 保持字面意义；适配器只利用原 token 的地址身份定位终止符边界，不再次解析选项值。
- 其它 alib6 限制（WP8 发现，未改）：`import std/alib6` 与文本 `#include` 标准库头在同一 TU（GCC 16）互相重定义 → **所有 `#include` 必须排在所有 `import` 之前**；`to_adata` 把枚举输出为标识符（`Mkdir`），规格要小写 → CLI 侧手工映射。

## 7. 已知的"悄悄不一样"之处（审阅重点）

1. **大小写不敏感只存在于 shim 的 Win32 API 里**（`CreateFileW`/`GetFileAttributesW`/`FindFirstFileW`/ini/版本资源经 `resolve_ci`）。上游通过 **Qt（`QFile`/`QDir`）** 做的文件访问在 Linux 上仍然区分大小写。游戏目录里写法固定，通常无碍，但 mod 目录名与 modlist 大小写不一致时 `list_mods` 已做 CI 兜底，其它路径没有。
2. **`SHGetKnownFolderPath` 默认返回 Unix 路径**（环境变量 `MOL_SHIM_UNIX_PATHS` 未设或为 1），因为上游随后用 Qt 拼路径，Windows 风格路径会被当成相对路径。设为 0 才返回 `C:\users\…`。
3. **`write_modlist` 会丢掉 `*` 开头的"未管理"行**（`read_modlist` 跳过它们）。MO2 加载时会自己重建，但优先级信息会丢失 —— 需要时应补保留逻辑。
4. **进程启动类 API 不实现**（`CreateProcessW/ShellExecute*` 恒失败）；启动由 `core/runner` 负责。
5. **uibase 日志默认无输出**（console sink 依赖 `GetConsoleMode`，shim 恒返回 0）。
6. **注册表来自 Wine 的 `system.reg/user.reg`**（只读，WP5 实现）；前缀未配置时一律"键不存在"，所以游戏路径必须由实例配置显式给出，不能指望 `detectGame()`。
7. **ImageNtHeader** 对未登记指针按 4KB 兜底做边界检查。

## 7.5 实测发现

- **`SHFileOperationW` 必须支持 `FO_COPY/FO_MOVE/FO_RENAME`**：上游 `copyToProfile` 经 `shellCopy` 走它，我们最初只实现了 `FO_DELETE`，复制失败后上游**静默创建空文件**（`initializeProfile` 得到 0 字节的 plugins.txt/ini）。已补齐并加 5 组测试；教训：shim 里"未实现"的函数，上游可能把失败吞掉而不是报错，今后新增 shim 功能要重点审视"失败是否会被静默吞掉"。

- **include 顺序陷阱的真实后果**：uibase 自己的 TU 若用 `-I include/uibase`，`pch.h` 里 `<string.h>` 在 `extern "C"` 块内 `#include <strings.h>` 会截获 uibase 的 `strings.h`，导致 `MOBase::ireplace_all/iequals` 被编成 **C 链接**，使用方 `dlopen` 时 `undefined symbol`。已改为对 uibase 自身与消费者统一 `-idirafter`，并用 `-iquote` 保证其自身 `"strings.h"` 解析正确（`host/CMakeLists.txt`）。
- **本机 Skyrim 状态**：`SkyrimSE.exe` 为 1.7.104.0（文件日期 9 月 9 日），目录里的 SKSE 是 `skse64_1_6_1170.dll`（loader 0.2.2.6）→ **版本不匹配，SKSE 多半无法加载**。属于 `doctor` 应报告的典型问题；先用无 SKSE 的 `SkyrimSE.exe` 验证启动。

- **真实数据上的农场**（本机 Skyrim SE，176 个文件/8 个目录）：合并约 2–10ms、应用约 2ms、二次 plan 为 0（幂等）。同层 `data/` 与 `DATA/` 并存时合并器正确给出 `intra-layer casefold conflict` 警告。
- **Proton 实测**（Proton 9.0 (Beta)，一次性前缀）：`cmd /c dir` 能列出农场，Wine 通过链接读到真实大小；复制 `Skyrim.esm`（249752131 字节）经由链接与原文件 `cmp` 一致。

## 8. 构建与运行

依赖（Arch）：`cmake>=4.4 ninja gcc>=16 clang qt6-base qt6-declarative spdlog zlib lz4 nlohmann-json glm rapidjson tomlplusplus`；`~/Projs/aaaa0ggmcLib`（或 `-DMOL_ALIB6_DIR=`）；`third_party/uibase`、`third_party/game_bethesda` 需检出（目前是本地 clone，**尚未建 submodule/固定 commit**）。

```bash
cmake -S . -B build -G Ninja                       # 含 host（clang 子工程，ExternalProject）
cmake -S . -B build -G Ninja -DMOL_BUILD_HOST=OFF  # 不含 host
cmake --build build -j8
ctest --test-dir build --output-on-failure
```
- `import std` 需要 CMake 实验开关（`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` 的 UUID **随 CMake 版本变**，当前写死 4.4 的值，见根 `CMakeLists.txt` 与 `docs/ALIB6_NOTES.md`）。
- 开发辅助：`scripts/syntax_check.sh <文件>` 对上游源码做 `-fsyntax-only` 快速迭代（注意它强制 `-include pch.h`，会掩盖缺失的 include，**以 host 的真实构建为准**）。

## 9. CLI 契约

完整规格见 [`docs/CLI.md`](docs/CLI.md)（全局选项、envelope、退出码 0/1/2/3、事件格式、各命令 `data` 字段）。要点：stdout 仅结果；日志走 stderr（alib6 `Logger`）；变更命令幂等并返回 `changed`；JSON 键按字典序（`sort_asc`）以保证输出确定。

## 10. 工作包台账

| WP | 内容 | 执行 | 结果 |
|---|---|---|---|
| WP0 | 骨架、shim 头文件、uibase 补丁、host 构建 | 副总监 | 完成 |
| WP1 | casefold 合并 | opencode | 通过，我加了对抗用例复核 |
| WP2 | MO2 格式解析 | opencode | 通过 |
| WP3 | 链接农场 | opencode | 通过 |
| WP4 | alib6 的 CMake 接入 | opencode | 通过（冷构建 25.8s） |
| WP5 | shim：ini / 版本资源 / 注册表 | opencode | 通过；我复跑并加了 ASan/UBSan 复核（它自带 PE 随机翻转 fuzz 与并发测试） |
| WP6 | shim：文件/映射/时间/shell/COM | opencode | 通过；我加了 ASan/UBSan 复核 |
| WP7 | 实例模型 | 副总监自写 | 完成 |
| WP8 | CLI 外壳 | opencode + Codex 独立复核与修复 | **完成**。复用原 WP8 session 接续一轮，移除选项自解析、公共规格表 + CommandInput 适配、参数数量检查、Logger→stderr、`game info`；Codex 修复编译/终止符/错误开关/事件与 apply 边界，并完成构建、14/14 单测与端到端验收。 |
| — | runner、game_host | 副总监自写 | 完成（未真实启动游戏；`proton run cmd` 级别的容器实测已做） |
| — | host 的 C ABI 扩展、`SHFileOperationW` COPY/MOVE/RENAME、uibase 的 include 修复 | 副总监自写 | 完成，见 §7.5 |

Claude 阶段的提交带 `Co-Authored-By` 与 `Claude-Session`；接续提交不冒用 Claude 身份。提交前跑 `privacy-scan.sh`（有 4 次命中均为测试里的占位路径，已逐条确认为误报，其中一次改为中性路径）。

## 11. 风险与未决

| 项 | 说明 | 严重度 |
|---|---|---|
| R1 | **host 已验证到的范围**：`libmo-game` 在本机真实 Skyrim SE 上 `create/info/mappings/initializeProfile/aboutToRun` 均正确（`gameVersion`=1.7.104.0 已用 `strings -e l SkyrimSE.exe` 独立核对；`initializeProfile` 与 `prepareIni` 在假前缀上验证）。**仍未验证**：`GamePlugins::writePluginLists`、存档读取（`SaveGameInfo`）、`DataArchives`、`LocalSavegames`、`UnmanagedMods` 等会走更多 shim 路径的功能 | 中 |
| R2 | ~~Proton 启动未实测~~ **已验证（容器与环境变量部分）**：用一次性前缀（`/tmp`）跑 `proton run cmd /c dir`，`STEAM_COMPAT_MOUNTS` / `PRESSURE_VESSEL_FILESYSTEMS_RW` 的取值足以让容器看到农场与其链接目标。**未验证**：真正启动 `SkyrimSE.exe`（需要交互与用户确认） | 中 |
| R3 | ~~符号链接在 Wine/容器内的行为~~ **文件级已验证**：Wine 经农场内符号链接看到目标文件真实大小，复制出的 249MB `Skyrim.esm` 与原文件逐字节一致。**未验证**：SKSE 注入、游戏对自身目录的探测、游戏写入（新文件落在农场而非 Steam 目录；已有链接文件被就地修改会写穿到 Steam 目录——需要 overwrite 捕获机制，尚未实现） | 中 |
| R4 | `third_party` 未固定 commit / 未建 submodule；uibase 补丁与版本绑定 | 中 |
| R5 | `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` 的 UUID 随 CMake 版本变，升级会直接失败（有清晰报错） | 中 |
| R6 | GPL-3.0：复用 MO2 代码意味着本项目须以 GPL-3.0 发布，**尚未添加 LICENSE** | 中 |
| R7 | `mo2fmt` 缺 Ini/loadorder writer（nlohmann 已从 core 移除，**已解决**） | 低 |
| R8 | `write_modlist` 丢 `*` 行（见 §7-3） | 低 |
| R9 | **plugins.txt / ini / 存档的 profile 同步**：设计已明确、部分已实现。上游 `mappings()`（profile 的 `plugins.txt`/`loadorder.txt` → 游戏 AppData）、`initializeProfile()`、`prepareIni()` 已通过 C ABI 暴露（`mo_game_mappings_json / mo_game_initialize_profile / mo_game_about_to_run`），并在假前缀上实测：`initializeProfile` 复制出正确内容，`prepareIni` 正确追加 `[Launcher] bEnableFileSelection=1` 且保留原有内容。**CLI 侧尚未接线**：把 `mappings()` 物化为符号链接（游戏写 plugins.txt 时写穿到 profile 文件，与 MO2/usvfs 语义一致）、每 profile 的 ini/存档隔离、overwrite 捕获 | 中 |
| R10 | **Qt 文件访问不做大小写不敏感**的真实后果：游戏跑过后会生成 `Skyrim.ini`/`SkyrimPrefs.ini`（大写），上游 `initializeProfile` 用 Qt 判断 `skyrim.ini` 是否存在 → 判为不存在 → 回退到游戏默认 ini，忽略用户已有设置。缓解方案（任选其一，待做）：①host 在调用前建一个只含小写别名链接的影子 Documents 目录并临时覆盖 shim 的 Documents 路径（不碰用户前缀）；②core 自己实现这一步 ini 复制 | 中 |
| R11 | ~~WP8 尚未完整复核~~ **已解决**：当前主仓库完成独立构建、14/14 单测、端到端生命周期和真实 host 的假目录集成；选项/开关由 alib6 解析、日志进 stderr、用法错误不执行变更。 | 已解决 |
| R12 | **alib6 的 4 处修改未提交**，且第 3 点是行为变更；若作者在别处使用了"同时声明破折号别名与 name，并依赖裸 name 匹配"的写法会受影响 | 中 |

## 12. 路线图（建议顺序；已完成项已划去）

1. ~~收尾当前批次：WP5 → 合并 → 真实 Skyrim 的 `info`~~ 已完成。
2. ~~WP8 第二轮 → 合并 → CLI 端到端（含 `game info`）~~ 已完成。
3. `plugins sync`：把 `mo_game_mappings_json` 的映射物化为符号链接（profile 的 plugins.txt/loadorder.txt → 前缀 AppData），并在 `apply` 里调用 `initialize_profile`（新 profile）与 `about_to_run`（写 `bEnableFileSelection`）。先解决 R10 的 ini 大小写问题。
4. `run`：真实启动实验（先 `SkyrimSE.exe`，再 `skse64_loader.exe`）；**需用户确认**（会启动游戏、写 Wine 前缀）。
5. overwrite 捕获（游戏在农场里新建的文件移回 `overwrite/`）。
6. `doctor`（前缀、Proton、**SKSE 与游戏版本是否匹配**——本机现在就不匹配、大小写冲突、manifest 漂移）。
7. 固定 `third_party` 提交（submodule）、补 LICENSE（GPL-3.0）、`mo2fmt` 的 Ini/loadorder writer、uibase 日志转发到 alib6。
8. **后话**：Nexus（SSO/API key、`is_premium`、`nxm://` 处理、下载；需 libcurl）、mod 安装（7z + FOMOD）。

## 13. 审阅清单（可直接照着跑）

```bash
# 1) 核心逻辑与 CLI 之外的全部单测
cmake -S . -B /tmp/b -G Ninja -DMOL_BUILD_HOST=OFF && cmake --build /tmp/b -j8 && ctest --test-dir /tmp/b --output-on-failure

# 2) 上游游戏层确实零修改
git -C third_party/game_bethesda status --short        # 期望：空
git -C third_party/uibase status --short               # 期望：空（补丁只作用于构建副本）

# 3) host 构建（clang）
cmake -S host -B /tmp/h -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/h -j8

# 4) shim 内存安全
cmake -S . -B /tmp/a -G Ninja -DMOL_BUILD_HOST=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" && cmake --build /tmp/a --target test_shim_files test_shim_misc && /tmp/a/shim/test_shim_files && /tmp/a/shim/test_shim_misc

# 5) alib6：cmd/parser/router/adata 的 gtest（改动已在作者工作区，未提交）
#    用 CMake 接入 mol_alib6 后编译 ~/Projs/aaaa0ggmcLib/tests/alib6/{main,test_cmd,test_parser,test_router,test_adata}.cpp（需要 gtest）
#    期望 35 个全部通过

# 6) CLI 端到端（先读脚本，强制临时 HOME；全部操作仅使用 /tmp 假实例）
task_home=$(mktemp -d /tmp/mol-home.XXXXXX)
HOME="$task_home" XDG_CONFIG_HOME="$task_home/config" bash tests/e2e_cli.sh /tmp/b/mo-linux
HOME="$task_home" XDG_CONFIG_HOME="$task_home/config" python3 tests/check_cli.py /tmp/b/mo-linux /tmp/h/libmo-game.so
# 两项成功后可删除本次 task_home；不碰真实 HOME 或 Wine 前缀。

# 7) 真实 Skyrim 上的探测（只读；用假前缀以免写你的 Wine 前缀）
#    可用 CLI 的 game info；真实目录只读，实例与 prefix 使用 /tmp 假目录。tests/check_cli.py 覆盖纯假目录版本。
```
审阅时建议优先看：`core/src/merge.cpp`（规则 §5.1 是否都落实）、`core/src/linkfarm.cpp`（`apply` 中途失败的 manifest 处理、`Remove` 的非递归保护）、`shim/src/internal.cpp`（`to_unix_path`/`resolve_ci` 的边界）、`host/src/fake_organizer.cpp`（假 `IOrganizer` 的默认值是否会让游戏插件走到错误分支）、`patches/uibase/`。

## 14. 约定

- **PMR**：公共 API 不出现 `std::string/vector/filesystem::path` 作为返回值或成员；含 pmr 成员的 struct 必须 allocator-aware（见 `core/include/mol/pmr.hpp` 顶部）。
- **alib6 优先**：JSON/命令行/日志用 alib6；文档里有过时 API，**以头文件源码为准**。
- **错误码**稳定且不可随意改名（`core/include/mol/error.hpp`），GUI 会依赖。
- **提交**：`type(scope): 中文描述`；Claude 阶段保留原有 Co-Authored-By/Claude-Session，接续者不冒用该身份；提交前 `privacy-scan.sh`；下游不得自行 commit。
