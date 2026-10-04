# mo-linux HANDBOOK

> 面向**审阅与复核**：设计意图、关键取舍、当前进度、未验证项、可执行的检查清单。
> 状态快照：2026-10-04。凡标 **[已验证]** 的都有可重跑的命令；**[未验证]** 的没有，别当成事实。

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
| `core/.../game_host` | dlopen 封装 `libmo-game.so` | 完成；真实加载**未验证**（等 WP5） |
| `cli/` | 命令路由、envelope、events、各子命令 | **WP8 返工中** |
| `shim/include` | Windows 头文件 shim（`windows.h` 等；声明即契约） | 完成 |
| `shim/src` | `internal`（路径转换/CI 解析/errno）、`files`、`misc` | 完成（ASan/UBSan 通过） |
| `shim/src` | `ini`、`version`、`registry` | **WP5 进行中** |
| `host/` | clang 子工程：uibase 副本 + game_bethesda + 假 IOrganizer + C ABI | **能编译链接**；运行**未验证** |
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

## 6.5 对 alib6 的改动（在 `~/Projs/aaaa0ggmcLib`，工作区未提交，需作者自行提交）

`Command` 的命令行解析有两处缺陷，已按用户授权修复（`modules/alib6/core/cmd.cpp`、`include/alib6/core/cmd.cppm`，约 85 行；`tests/alib6/test_cmd.cpp` 新增 5 个用例，alib6 的 cmd/parser/router 共 17 个测试通过）：
1. **前置选项**：第一个路由 token 之前的选项/开关（`prog -j -i /x mods list`）此前不会让路由下降，整条路由被挤成位置参数。现在 `dispatch_pipeline` 在 `router.match` 前做"根层预扫描"，选项落入 main 层，`has/get` 跨层可见。
2. **`--` 终止符**：此前 `--` 后的 `-j` 仍被当作开关，且 `--` 进入 `args()`。现在 `--` 之后一律是位置参数，`--` 本身被吞掉（含前置位置）。
- 实验结论（可复核）：选项在第一个路由 token 之后放哪里都行；`--name=value` 与 `--name value` 均支持；`has()` 扫描所有层，`get()` 从当前层往上回溯。
- 因此 CLI 层**不需要**自己解析选项；WP8 当初自写的 `cli/args.cpp` 应删除（见 §10 WP8）。

## 7. 已知的"悄悄不一样"之处（审阅重点）

1. **大小写不敏感只存在于 shim 的 Win32 API 里**（`CreateFileW`/`GetFileAttributesW`/`FindFirstFileW`/ini/版本资源经 `resolve_ci`）。上游通过 **Qt（`QFile`/`QDir`）** 做的文件访问在 Linux 上仍然区分大小写。游戏目录里写法固定，通常无碍，但 mod 目录名与 modlist 大小写不一致时 `list_mods` 已做 CI 兜底，其它路径没有。
2. **`SHGetKnownFolderPath` 默认返回 Unix 路径**（环境变量 `MOL_SHIM_UNIX_PATHS` 未设或为 1），因为上游随后用 Qt 拼路径，Windows 风格路径会被当成相对路径。设为 0 才返回 `C:\users\…`。
3. **`write_modlist` 会丢掉 `*` 开头的"未管理"行**（`read_modlist` 跳过它们）。MO2 加载时会自己重建，但优先级信息会丢失 —— 需要时应补保留逻辑。
4. **进程启动类 API 不实现**（`CreateProcessW/ShellExecute*` 恒失败）；启动由 `core/runner` 负责。
5. **uibase 日志默认无输出**（console sink 依赖 `GetConsoleMode`，shim 恒返回 0）。
6. **注册表来自 Wine 的 `system.reg/user.reg`**（只读，WP5 实现）；前缀未配置时一律"键不存在"，所以游戏路径必须由实例配置显式给出，不能指望 `detectGame()`。
7. **ImageNtHeader** 对未登记指针按 4KB 兜底做边界检查。

## 7.5 实测发现

- **include 顺序陷阱的真实后果**：uibase 自己的 TU 若用 `-I include/uibase`，`pch.h` 里 `<string.h>` 在 `extern "C"` 块内 `#include <strings.h>` 会截获 uibase 的 `strings.h`，导致 `MOBase::ireplace_all/iequals` 被编成 **C 链接**，使用方 `dlopen` 时 `undefined symbol`。已改为对 uibase 自身与消费者统一 `-idirafter`，并用 `-iquote` 保证其自身 `"strings.h"` 解析正确（`host/CMakeLists.txt`）。
- **本机 Skyrim 状态**：`SkyrimSE.exe` 为 1.7.104.0（文件日期 9 月 9 日），目录里的 SKSE 是 `skse64_1_6_1170.dll`（loader 0.2.2.6）→ **版本不匹配，SKSE 多半无法加载**。属于 `doctor` 应报告的典型问题；先用无 SKSE 的 `SkyrimSE.exe` 验证启动。

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
| WP5 | shim：ini / 版本资源 / 注册表 | opencode | **进行中**（超出 shell 预算，持续迭代） |
| WP6 | shim：文件/映射/时间/shell/COM | opencode | 通过；我加了 ASan/UBSan 复核 |
| WP7 | 实例模型 | 副总监自写 | 完成 |
| WP8 | CLI 外壳 | opencode | **返工中**：撤销手写 JSON，改用反射；做 e2e 联调。**下一轮**：删除 `cli/args.cpp` 的选项自解析，直接读 `CommandInput`（alib6 的 Command 已修） |
| — | runner、game_host | 副总监自写 | 完成（未真实启动） |

提交均带 `Co-Authored-By` 与 `Claude-Session`；提交前跑 `privacy-scan.sh`（有 4 次命中均为测试里的占位路径，已逐条确认为误报，其中一次改为中性路径）。

## 11. 风险与未决

| 项 | 说明 | 严重度 |
|---|---|---|
| R1 | ~~host 运行时未验证~~ **已验证（部分）**：`libmo-game` 已能 `dlopen`，`GameSkyrimSE::init/setGamePath` 与全部信息查询在本机真实 Skyrim SE 目录上返回正确结果（目录、DLC、可执行文件、SKSE、ini 名、`gameVersion`=1.7.104.0，已用 `strings -e l SkyrimSE.exe` 独立核对）。**仍未验证**：`initializeProfile`、`GamePlugins::writePluginLists`、存档读取等会走更多 shim 路径的功能 | 中 |
| R2 | **Proton 启动未实测**：`proton run` 在 Steam 之外的环境变量集合、pressure-vessel 能否解析指向农场外的符号链接（我用 `STEAM_COMPAT_MOUNTS`/`PRESSURE_VESSEL_FILESYSTEMS_RW` 兜底）均未实测 | 高 |
| R3 | **符号链接在 Wine/容器内的行为**：农场里 exe 与 DLL 是链接；SKSE、`GetModuleFileName`、游戏对自身目录的探测是否正常，未验证 | 高 |
| R4 | `third_party` 未固定 commit / 未建 submodule；uibase 补丁与版本绑定 | 中 |
| R5 | `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` 的 UUID 随 CMake 版本变，升级会直接失败（有清晰报错） | 中 |
| R6 | GPL-3.0：复用 MO2 代码意味着本项目须以 GPL-3.0 发布，**尚未添加 LICENSE** | 中 |
| R7 | `mo2fmt` 缺 Ini/loadorder writer（nlohmann 已从 core 移除，**已解决**） | 低 |
| R8 | `write_modlist` 丢 `*` 行（见 §7-3） | 低 |
| R9 | plugins.txt / ini / 存档与 Wine prefix 的同步（每 profile 隔离）**尚未设计落地** | 高（功能缺口） |

## 12. 路线图（建议顺序）

1. **收尾当前批次**：WP5 → 合并 → 跑通 `libmo-game` 对真实 Skyrim 目录的 `info`（我本机有 `~/.steam/steam/steamapps/common/Skyrim Special Edition` 与 `compatdata/489830`）。
2. WP8 返工 → 合并 → CLI 端到端。
3. `game info`、`plugins sync`（用上游 `GamebryoGamePlugins` 写 `plugins.txt` 到前缀 AppData）、profile 的 ini/存档隔离（R9）。
4. `run`：真实 Proton 启动实验（先 `SkyrimSE.exe`，再 `skse64_loader.exe`）→ 固化 R2/R3 的结论。
5. `doctor`（检查前缀、Proton、SKSE 版本匹配、大小写冲突、manifest 漂移）。
6. 固定 `third_party` 提交、补 LICENSE、`mo2fmt` writer、uibase 日志转发到 alib6。
7. **后话**：Nexus（SSO/API key、`is_premium`、`nxm://` 处理、下载；需 libcurl；见对话结论）、mod 安装（7z + FOMOD）。

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

# 5) alib6 反射序列化可用（见 WP8 返工 brief 的片段）
```
审阅时建议优先看：`core/src/merge.cpp`（规则 §5.1 是否都落实）、`core/src/linkfarm.cpp`（`apply` 中途失败的 manifest 处理、`Remove` 的非递归保护）、`shim/src/internal.cpp`（`to_unix_path`/`resolve_ci` 的边界）、`host/src/fake_organizer.cpp`（假 `IOrganizer` 的默认值是否会让游戏插件走到错误分支）、`patches/uibase/`。

## 14. 约定

- **PMR**：公共 API 不出现 `std::string/vector/filesystem::path` 作为返回值或成员；含 pmr 成员的 struct 必须 allocator-aware（见 `core/include/mol/pmr.hpp` 顶部）。
- **alib6 优先**：JSON/命令行/日志用 alib6；文档里有过时 API，**以头文件源码为准**。
- **错误码**稳定且不可随意改名（`core/include/mol/error.hpp`），GUI 会依赖。
- **提交**：`type(scope): 中文描述` + Co-Authored-By/Claude-Session；提交前 `privacy-scan.sh`；下游不得自行 commit。
