# 提案：模组影响面分析（injection points → impact scope）

状态：**提案**。动机：2026-10-10 用户问「能不能分析每个模组的注入地点，从而分析出它影响的范围」。
这条线与审计文档里那条原则直接相接——**影响面 ≠ 责任**：知道一个模组*能*碰什么，
不等于崩溃堆栈里出现它就有罪。本文只回答「能碰到什么」，永远不回答「谁是肇事者」。

## 1. 「注入地点」的分类学

一个 Skyrim SE 模组能进入游戏进程/写入磁盘的位置，按**作用域**从窄到宽：

| 注入地点 | 典型路径 | 谁加载它 | 作用域 |
|---|---|---|---|
| SKSE 插件 | `SKSE/Plugins/*.dll` | SKSE（游戏进程内） | 仅游戏进程 |
| 引擎/脚本扩展 DLL | `Data/**/*.dll`（被 Papyrus/其它插件按需加载） | 游戏或其它插件 | 游戏进程（可能被链式加载） |
| **代理 DLL** | mod 根或游戏根的 `d3d11.dll`/`dxgi.dll`/`version.dll`/`winhttp.dll`/`winmm.dll`/`dinput8.dll`/`xinput1_3.dll`/`nvwgf2umx.dll`… | **Windows 加载器**（任何加载游戏 EXE 的进程） | **所有进程**（含 Steam 覆盖层、启动器） |
| EXE 工具 | Nemesis/FNIS/BodySlide/xLODGen/DynDOLOD/FOMOD Installer | 用户手动/脚本 | 离线：只写文件，不改运行态 |
| Papyrus 脚本 | `Data/Scripts/*.pex`(源 `.psc`) | 游戏虚拟机 | 游戏逻辑 |
| 内容插件 | `*.esp/.esl/.esm` | 游戏 | 内容（已有 `conflicts` 覆盖文件层） |
| 配置 | `*.ini`/`*.json`/`*.toml` | 游戏/插件 | 行为开关 |

「代理 DLL」是最容易被低估的一类：它在**每个**加载游戏 EXE 的进程里都跑，包括用户根本没意识到的
启动链。ENB 的 `d3x11.dll`、各种 overlay、甚至一些「优化」模组都走这条路。

## 2. 从注入地点推导「影响范围」

三个正交的轴，全部可以静态算出：

### 2.1 Reach（作用域）—— 从注入地点直接得出
- SKSE 插件 / Data DLL → `game-process`
- 代理 DLL → `all-processes`（注入地点本身就说明问题）
- EXE 工具 → `offline`（只写文件）
- Papyrus / ESP → `game-logic` / `game-content`

### 2.2 Capability（能力面）—— 从 PE 导入表得出
解析 DLL 的 import directory，按 DLL 名 + 函数名归类：

| 能力 | 信号（导入的 DLL::函数） |
|---|---|
| 写文件 | `kernel32!CreateFileW`(GENERIC_WRITE)/`WriteFile`/`DeleteFile`/`MoveFile`/`CreateDirectory` |
| 起进程 | `kernel32!CreateProcessW`/`ShellExecuteW`/`WinExec`/`system` |
| 网络 | `ws2_32!socket/connect/WSAStartup`/`winhttp!*`/`wininet!*` |
| 改注册表 | `advapi32!RegSetValue*`/`RegCreateKey*`/`RegDelete*` |
| 改内存/hook | `kernel32!VirtualProtect`/`WriteProcessMemory`/`ReadProcessMemory` |
| 链式加载 | `kernel32!LoadLibrary*`/`LdrLoadDll` |
| 读写进程 | `toolhelp!CreateToolhelp32Snapshot`/`Thread32First`… |

### 2.3 Declaration（自声明）—— 从 PE 导出表 + 资源得出
- 导出 `SKSEPluginVersion`/`SKSEPluginQuery`/`SKSEPluginLoad` → 是 SKSE 插件（比看路径更可靠）。
- 导出表里的版本结构体给出**声明的兼容运行时版本**（`SKSEPluginVersion` 的 versionIndependence 等）。
- `VS_FIXEDFILEINFO`（`pe_file_version()` 已在 health.cpp 有）给出文件版本。
- 有没有 `.bindata`/可疑节（高熵、可写+可执行）→ 加壳/自修改的线索。

### 2.4 合成「影响范围」的自然语言结论
例：
- `EngineFixes.dll` → SKSE 插件 + 写内存 + 改注册表 → 「游戏进程内；会 patch 引擎内存；可能写前缀注册表」。
- `enbseries` 的 `d3d11.dll` → 代理 DLL → 「**所有**加载游戏 EXE 的进程；着色器编译（我们在审计里追过的 E5020 就是它）」。
- `FNIS.exe` → EXE 工具 → 「离线；只写 `Data/meshes/...` 的动画文件」。

## 3. 静态分析的边界（必须写进文档，不能装作全知）

1. **导入表 ≠ 行为**：只反映链接期依赖；`GetProcAddress` 动态解析、手动 syscall、加壳都能绕开分类。
   能力面是**上界**，不是行为清单。
2. **Hook 目标是运行时的**：SKSE trampoline、vtable patch、detour 在静态层面看不到目标函数。
   我们知道「它会改内存」，不知道「改的是哪段引擎代码」——后者需要动态追踪（另一件事）。
3. **会被骗**：DLL 可以改名（把 `version.dll` 命名为别的、或反之）；代理 DLL 也可能是正经渲染器 mod。
   所以分类要**以导出表/PE 结构为主、路径名为辅**，并且对「路径名与 PE 内容矛盾」的情况明确标注。
4. **影响 ≠ 责任**：这是审计文档第 3 节的延续。崩溃堆栈里出现某模组 ≠ 它是肇事者；
   本分析只用于「这个模组*能*碰什么」，用于理解、审查和缩小排查范围，
   永远不作为自动停用某模组的依据。

## 4. 落地设计

### 4.1 新 primitive：`mol::pe`（PE 解析）
现有两块可复用：`health.cpp` 的 `pe_file_version()`、`lua_script.cpp` 的 `pe_machine()`。
新增：import directory（DLL 名 + 函数名/序号）、export directory（名字 + 序号）、节表（名字 + 特征）。
只读、有界（目录大小/条目数上限）、坏文件返回 nullopt——**绝不**因为一个畸形 DLL 让整轮分析失败。

### 4.2 新模块：`mol::impact`
```cpp
struct InjectionPoint {            // 一个注入地点
    string kind;                   // "skse_plugin" | "proxy_dll" | "engine_dll" | "exe_tool" | "papyrus" | "content" | "config"
    string path;                   // 相对 mod 根的路径
    string loaded_by;              // "skse" | "windows_loader" | "engine" | "user" | "game_vm"
    string reach;                  // "game-process" | "all-processes" | "offline" | "game-logic" | "game-content"
};
struct Capabilities {              // 能力面（bool 集 + 证据）
    bool writes_files, spawns_processes, network, registry, memory_patch, chain_loads;
    vector<string> evidence;       // "kernel32!WriteFile" 这样的导入证据（≤16 条）
};
struct ModImpact {
    string mod;
    vector<InjectionPoint> injections;
    Capabilities caps;
    bool packed_suspect = false;   // 节特征像加壳（可写+可执行、名字异常）
    string summary;                // 一句人话：「SKSE 插件；会 patch 内存；仅游戏进程」
};
vector<ModImpact> analyze_mods(const Instance&, span<const string> mods, mr*);
```

### 4.3 CLI：`mods impact [NAME]`
- data：`{mods:[{mod, injections:[{kind,path,loaded_by,reach}], caps:{writes_files,…}, packed_suspect, summary}]}`
- 文本模式每模组几行，人可直接读。
- 全部只读、可重复；单个 DLL 解析失败只降级该条目（记 warning），不影响其它模组。

### 4.4 Doctor 集成（规则层事实，与 `enb.compiler_log` 同思路）
把影响面事实喂给 Lua 规则，让「已知问题」的判断有据可依：
- `impact.<mod>.reach = all-processes` + 与崩溃相关 → 提示「注意：这个模组在所有进程里」。
- `impact.<mod>.caps.memory_patch` + 偶发 TBB/allocator 崩溃 → 提示「它会改内存，请用可回滚 profile 对照」。
- `impact.<mod>.packed_suspect` → 「DLL 可能加壳，静态分析看不到它的导入」。
**不**自动停用任何模组（§3.4）。

### 4.5 与 Lua 脚本层的关系
`mol::impact` 是纯只读分析，可以（也应该）暴露给安装脚本：脚本在 `mods.install_*` 之后
调用 `impact.of(name)` 做**安装后自检**——「我装进来的东西声明自己是 SKSE 插件但导出表里没有
SKSEPluginVersion」，或者「这个 mod 带代理 DLL，会影响所有进程」。这正好补上「从 0 到 100 安装」
里缺失的验证环节。

## 5. 分阶段与验收

### WP-A（提案即本阶段）：PE primitive + 注入地点分类
`mol::pe`（imports/exports/sections）+ 按路径/导出表分类注入地点 + `mods impact` CLI。
验收：对一个真实实例（Constellations 的 2444 个 mod 里挑 30 个）跑出来：
SKSE 插件、代理 DLL、EXE 工具、Papyrus 分类正确；畸形 DLL 不炸；全量 < 5s。

### WP-B：能力面 + summary
导入表归类成能力集 + 证据 + 一句人话 summary + `packed_suspect`。
验收：已知样本（EngineFixes=内存 patch、某 overlay=网络+代理 DLL、FNIS=离线）分类正确；
`GetProcAddress`-only 的 DLL 被标为「能力面上限不可知」而不是误报为「什么都不做」。

### WP-C：doctor 事实 + 脚本 API
`impact.*` 事实进 rules；`impact.of()` 进 Lua 脚本。
验收：规则能在「代理 DLL 模组 + 崩溃」时给出**提示而非停用**；脚本能在安装后自检导出表。

## 6. 明确不做

- **不做动态追踪**：hook 目标、实际系统调用序列需要另一套机制（etrace/ptrace），不在本提案范围。
- **不做「谁是肇事者」的自动判定**：见 §3.4，这是原则问题不是能力问题。
- **不做脱壳/反混淆**：加壳 DLL 只标注「不可静态分析」，不尝试解开。
