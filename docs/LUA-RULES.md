# Lua 规则层（Lua rules）

Lua 规则层让「已知兼容问题、版本范围、mod 互斥、偏好相关的可选方案」用脚本来表达，
而不必每加一条判断就改 C++。它只做**只读体检**：收集一份事实快照，跑规则，产出结构化
诊断（`Check`）。解包、文件遍历、插件二进制解析、farm diff、COW、备份事务与进程管理仍留在 C++。

设计动机与完整提案见 [`DESIGN-lua-rules.md`](./DESIGN-lua-rules.md)（本文只描述**已落地**的接口与边界）。

## 运行时

- **Lua 5.4 ABI（固定）**，通过 **vendored sol2 v3.3.0** 绑定；链接系统的 Lua 5.4 库，不使用系统默认 Lua 版本，也不开放原生模块加载。
- 每条规则在**各自独立、有界的 Lua state** 里运行（`lua_newstate` + 自定义分配器 + 计数 hook）。
  一条脚本崩溃、超时或超限，只折叠成它自己的一条警告，**不影响其它规则**。
- Lua 侧**没有** `io` / `os` / `package` / `debug`，也没有 `load` / `dofile` / `loadfile` / `require` /
  `pcall` / `xpcall` / `setmetatable` / `collectgarbage`。只保留必要的 base、以及裁剪过的
  `string` / `table` / `math`。因此规则**不能**读写文件、起进程、联网或执行任意命令。

字面量查找使用 `string.find(text, needle, 1, true)`（文本最多 128 KiB，查找词最多 4096 字节）。
`string.match/gmatch/gsub/format/dump` 与 `table.move` 不开放；常用 table 序列操作限制为 16384 项。
原生 C 函数不能用 Lua 指令 hook 中断，因此这些限制独立于指令预算。

## 规则从哪里来

1. **内置规则**：随二进制发布（编译期嵌入），覆盖 Skyrim 的样板问题。
2. **实例规则**：`<实例根>/rules/*.lua`，按文件名排序、确定性地加载（最多 32 个，超出记一条警告）。

两类规则的输出都会追加到 `mo-linux doctor`（`run_doctor` 末尾调用 `rules::run`），并流入
`mo-linux next`：带 `fix` 的诊断变成可执行步骤，不带 `fix` 的变成 `needs_human` 步骤。

## 规则 API（v1）

一个脚本返回一张表：

```lua
return {
  api_version = 1,                 -- 必须为 1，否则整条被拒
  id = 'skyrim.map.player_location',-- 稳定 id，只允许 [A-Za-z0-9._-]，≤128 字符
  check = function(ctx)
    -- ctx = {
    --   game = 'skyrimse', game_version = '1.6.1170.0',
    --   facts = { ['preferences.allow_disable_mod'] = 'true', ... },  -- key=value（都是字符串）
    --   mods = { { name=, version=, enabled=, exists=, nexus_id= }, ... },  -- 1 起算的数组
    --   impact = { { mod=, packed_suspect=, summary=,                 -- 启用且存在的 mod 的影响面
    --                 injections = { {kind=,path=,loaded_by=,reach=}, ... },
    --                 caps = {writes_files=,spawns_processes=,network=,registry=,
    --                         memory_patch=,chain_loads=,unknown=} }, ... },
    -- }
    return {
      {
        id = 'map.player_location.hidden',   -- 与 rule id 拼成 lua.<ruleid>.<id>
        level = 'warn',                      -- ok | warn | error
        message = 'Get Lost hides the player location on the world map',
        hint = 'disable Get Lost if you want the player marker',   -- 可选
        -- 需要停用时，根据下文的偏好和 mod 状态条件添加 action。
      },
    }
  end,
}
```

- 每条检查的最终 id 规整为 `lua.<ruleid>.<id>`（稳定、可被 `next`/GUI 依赖）。
- `check` 返回一个**数组**；数组长度超过 `checks` 上限（默认 64）时整条规则被拒。
- `message` / `hint` 是给人看的文本；不要把日志原文或密钥塞进来（见「错误行」）。

### 动作（action）

唯一支持的动作是**停用 mod**：

```lua
action = { type = 'disable_mod', name = '<精确的 mod 名>' }
```

只有当**同时**满足以下条件，运行时才把它变成一个修复 argv `["mods","disable","<name>"]`：

- 动作类型是 `disable_mod`；
- `ctx.facts["preferences.allow_disable_mod"] == "true"`（用户显式 opted in）；
- `<name>` 与某个**已存在、已启用、非分隔符**的 mod **精确同名**。

任一条件不满足（或出现任何其它动作类型，例如设想中的 `execute_shell` / `set_launch_environment`），
整条规则被拒并折叠成一条 `lua.runtime` 警告——**不会**执行任何命令，也**不会**从 Lua 侧接受路径或
任意参数。Lua 只「提议」动作，是否可执行由宿主判定。

### 资源与输入上限（`Limits`）

| 上限 | 默认 | 超出后的行为 |
|---|---|---|
| `source_bytes` 脚本大小 | 128 KiB | 该脚本被拒 → `lua.runtime` 警告 |
| `memory_bytes` Lua 堆 | 8 MiB（夹到 [256 KiB, 32 MiB]） | 分配失败 → 该规则被拒 → 警告 |
| `instructions` 指令预算 | 2000000 | 计数 hook 触发 → 该规则被拒 → 警告 |
| `checks` 单规则检查数 | 64 | 该规则被拒 → 警告 |

无限循环、内存炸弹都会被这些上限截断成一条警告，**不会挂死或 OOM**。

## 内置检查与事实

`rules/skyrim.lua` 检查 Get Lost 地图位置偏好、ENB `E5020 / fx_5_0`、两组可选模式冲突，
以及四类**注入地点/能力面**告知（`injection.proxy_dll` / `injection.chain_dll` /
`injection.network` / `injection.packed`）。ENB 只报告证据，不自动修改配置。
`enb.compiler_log` 来自前缀中的
`AppData/Local/KiLoader/SkyrimSE/Logs/KiENBExtender.log`，只读取最多 64 KiB；
存在 SKSE 的 `skse64_loader.log` 启动标记且 ENB 日志不早于该标记时才提供此事实。

`ctx.impact` 是 `mods impact` 同一套静态分析（见 [`DESIGN-mod-impact.md`](./DESIGN-mod-impact.md)）
喂给规则的事实：每个**启用且存在**的 mod 一条，含注入地点（`skse_plugin` / `proxy_dll` /
`engine_dll` / `exe_tool` / `papyrus` / `content` / `config` / `other`）、作用域
（`game-process` / `all-processes` / `offline` / `game-logic` / `game-content` / `none`）
与能力面（导入表归类，是**上界**）。收集带磁盘缓存（按 mod 目录 mtime 失效，
`~/.cache/mo-linux/impact/`）：2400 个 mod 的实例第一次约 1 分钟，之后每次约 1 秒。
四类注入检查**只告知、永不停用**（影响 ≠ 责任），也不给 `fix`。

## 偏好（`rules.ini`）

实例偏好写在 `<实例根>/profiles/<profile>/rules.ini`：

```ini
[Preferences]
ShowPlayerWorldmapPosition=true
AllowDisableMod=true
```

`collect_context` 把这些读成事实 `preferences.*`（布尔一律以字符串 `"true"` / `"false"` 表示）。
偏好是**选择性加入（opt-in）**的：没有 `AllowDisableMod=true` 时，即使检测到问题，规则也只会给诊断、不会提议停用 mod。
（见 `tests/test_rule_facts.cpp`。）

## 独立游戏描述（`describe`）

CLI 可用 `mo-linux -j game describe games/generic-example.lua`；无需实例或 MO2 host。

`describe` 让脚本描述一个**不在 MO2 里**的游戏，返回 `{id, executable, data_directory, plugin_format}`：

```lua
return {
  api_version = 1,
  id = 'fake.game',
  executable = 'bin/Game.exe',     -- 相对路径
  data_directory = 'data',         -- 相对路径
  plugin_format = 'tes',           -- 仅 'none' 或 'tes'
}
```

- `executable` / `data_directory` 只接受**相对路径**：拒绝绝对路径、`..` 穿越、盘符（`:`）与反斜杠。
- `plugin_format` 只能是 `none` 或 `tes`。
- 任何不合法输入都抛 `std::runtime_error`（由 C++ 调用方捕获）。

这是一项**基础能力**：它只「描述」一个游戏的布局与插件格式，**不是**一个完整的通用启动适配器。
真正的探测、映射与启动执行仍在 C++（现有 MO2 host 保持不变）；新游戏若需要新归档/二进制格式，
仍要先加 C++ primitive，再供 Lua 调用。

## 边界与承诺

- **停用 ≠ 部署**：`disable_mod` 只改 `modlist.txt`（`mo-linux mods disable`）。要让农场/链接树反映这次
  停用，仍需 `mo-linux apply`。规则不会替你应用部署。
- **不包治崩溃**：规则只会呈现「有可靠检测、明确目标、已验证修复」的问题。它不会把崩溃堆栈里的模块名
  自动当成肇事者，不会把玩家的玩法选择当成安装错误，也**不承诺**修好崩溃。偶发崩溃先给证据与验证建议。
- **无任意命令**：Lua 不能起进程、读写文件、联网或拼 CLI 字符串执行；只有宿主验证过的 `disable_mod`
  能变成一条 `mods disable` 步骤。
- **顺序**：规则文件按名排序，诊断必须是连续数组，按数组顺序输出；id 稳定。规则作者应避免依赖 `pairs` 的顺序或随机数。

## 错误行

运行期把失败/超额的规则折叠成一条检查：`id = "lua.runtime"`、`level = "warn"`，
`hint` 给可操作的下一步（「检查该规则的 API/schema 与资源上限；其它规则独立继续：`<规则名>`」）。
错误行只给**可操作提示**，不倾倒原始日志、不泄露密钥或路径细节。

## 测试

`tests/test_rules.cpp` 覆盖：合法检查与 id 前缀、`ctx` 字段、非法 schema/API、无限循环与内存炸弹有界、
sandbox 缺失的库、source/checks 上限、任意 action 被拒、`disable_mod` 门控、坏规则不牵连下一条、
`describe` 的合法与各类拒绝。除 `collect_context` 用 `/tmp` 临时实例外，夹具全部在内存中。

CLI 离线联调：`HOME=/tmp/mol-test XDG_CONFIG_HOME=/tmp/mol-test/config python3 tests/check_lua_cli.py /path/to/mo-linux`。该脚本拒绝在真实 HOME 下运行。
