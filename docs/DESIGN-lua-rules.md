# 提案：Lua 规则层与游戏适配器

状态：首版实现采用 Lua 5.4 + sol2 v3.3.0，规则接入 doctor/next，实际 v1 API 见 [LUA-RULES.md](LUA-RULES.md)。本文保留后续适配器与动作扩展设计；下文拟定接口不等于首版已支持的能力。来源：2026-10-10 用户希望新增兼容规则和其他游戏适配不再不断修改 C++。

## 目标与边界

采用 C++ 核心 + Lua 规则/适配层是合适的方向。日志位置、已知兼容问题、版本范围、mod 互斥关系、游戏目录布局、启动参数和 profile 映射适合脚本。解包、文件遍历、插件二进制解析、farm diff、COW、备份事务与进程管理留在 C++。

规则能够自动处理“有可靠检测、明确目标、已验证修复”的问题；不能将崩溃堆栈中的模块名自动当成肇事者，也不能把玩家选择的玩法当成安装错误。Get Lost 隐藏地图位置应根据玩家的“显示玩家位置”目标提出停用方案；偶发 TBB 崩溃先给证据与验证建议。

## 当前接入点

- `core/include/mol/doctor.hpp` 的 Check 已有 id、level、message、hint、fix。
- `cli/cmd_agent.cpp::next_data()` 将 Check 转为步骤，但是否需要确认依赖命令元数据，无法表达同一操作的不同原因/验证级别。
- `core/include/mol/game_host.hpp` 已隔离原生 MO2 host；可以在其外增加统一 GameAdapter 接口，让 MO2 host 与 Lua adapter 并存。
- `health.cpp` 的日志目录、`steam_detect.cpp` 的游戏安装路径、`plugins.cpp` 的隐式插件列表仍有 Skyrim 常量。若只给 doctor 加 Lua，跨游戏目标仍未完成。

## 规则契约（拟定 v1）

每个脚本返回规则描述：`api_version`、`id`、`version`、`games`、`capabilities`、`check(context)`。宿主一次收集事实快照，规则只读它，并返回结构化诊断。

诊断包含稳定 id、severity、message、evidence（来源、时间、版本）、confidence、suggested_actions。每个 action 有类型、参数、前置条件和验证条件。动作类型先支持：

1. `set_mod_enabled`：限定实例和 profile，按已安装 mod 的稳定身份定位，名称仅用于显示。
2. `config_overlay`：指定虚拟路径与 INI/TOML 键值，生成独立修复 mod，不写原游戏文件。
3. `backup_prefix_file`：按宿主批准的路径范围做备份，复用既有事务实现。
4. `set_launch_environment`：限定允许设置的环境变量。
5. `manual_step`：记录需要凭据、外部下载或人工游戏验证的步骤。

不要开放 `execute_shell` 或“拼接一段 CLI 字符串直接运行”。Lua 提出动作，宿主验证、生成 dry-run、检查忙状态、执行与回滚。只有同时满足用户目标、前置条件与已验证策略的动作才能进入自动修复。

以下只是接口示例，不是当前可执行代码：

```lua
return {
  api_version = 1,
  id = "skyrim.map.player_location",
  version = 1,
  games = {"skyrimse"},
  capabilities = {"mods.read", "preferences.read"},
  check = function(ctx)
    local mod = ctx.mods:find_nexus_file(119736, 513962)
    if not mod or not mod.enabled then return {} end
    if ctx.preferences.show_player_worldmap_position ~= true then return {} end
    return {{
      id = "map.player_location.hidden",
      severity = "warning",
      message = "Get Lost hides the player location on the world map",
      confidence = "confirmed_configuration",
      evidence = {{kind = "enabled_mod", identity = mod.identity}},
      suggested_actions = {{
        type = "set_mod_enabled",
        mod = mod.identity,
        enabled = false,
      }},
    }}
  end,
}
```

文件 ID 在示例中标识本次确认的版本，后续支持其他版本应在规则中明确范围，不能默认行为永远相同。

## 游戏适配契约

GameAdapter 至少返回：游戏 id、安装探测提示（Steam app id 等）、版本探测策略、虚拟 Data 映射、日志根、profile 配置映射、启动器、插件格式与能力集合。

Lua adapter 可以独立描述不在 MO2 中的游戏；缺失能力必须显式报告。例如某游戏没有 TES 插件顺序，宿主不能仍执行 Skyrim 的 masters 检查。新游戏需要新归档格式或特殊二进制格式时，仍需新增 C++ primitive，然后供 Lua 调用。Lua 不等于所有游戏都能零 C++ 接入。

保留现有 MO2 adapter 作为 Skyrim 默认实现，不一次替换已经工作的路径。把日志位置等常量迁入 adapter 后，再增加一个离线假游戏 adapter，证明非 MO2 路径能够工作。

## 执行约束

用 Lua C API 嵌入，明确支持的 Lua 版本；不要随意跟随系统默认 Lua 版本。仅加载文本脚本，默认只提供 base 的必要部分与 string/table/math；不开放 io/os/debug/package 或原生模块加载。

通过自定义 allocator 限制内存，通过 instruction hook 限制指令预算；宿主 API 也限制输入大小与扫描量，因为 Lua hook 不能中断耗时的 C 函数。每个规则独立状态或隔离环境，脚本异常只禁用该规则并输出 `rules.failed`，不吞掉诊断失败。

读文件能力以宿主注册的逻辑根为单位，不能让 Lua 任意提供绝对路径；需检查穿越、symlink 逃逸与竞态。规则包由用户显式安装，不能执行合集压缩包中未经选择的 Lua 文件。单条规则的 capabilities 不能扩大宿主授权。

结果排序、id 冲突、api 版本不兼容、覆盖顺序都必须确定。将规则版本、证据时间、动作前置条件和修复记录存到实例，便于升级与回滚。跨实例/跨 profile 的共享 mod 文件不能被修复动作直接修改。

参考：[Lua 官方手册](https://www.lua.org/manual/5.4/manual.html) 的嵌入 C API、allocator、hook 与文本 chunk 加载机制。

## 分阶段交付与验收

### WP1：副总监定义公共类型与宿主边界

新增独立 rules 接口，先支持 facts → diagnostics → typed actions。保留现有 Check 和 next JSON，新增字段需版本化且兼容旧客户端。规则查找目录、来源优先级和 id 重复策略明确化。

验收：同一 facts 输出确定；无用户偏好时不会自动停用玩法 mod；过期日志不当成当前故障；未验证崩溃只生成诊断。

### WP2：opencode 实现 Lua runtime 和离线测试

在 WP1 接口固定后实现脚本加载、限额、错误隔离、能力 API；不改全局配置、游戏实例或 Lua/CMake 版本策略。

验收：无限循环、内存超额、路径逃逸、无权限 API、重复 id、错误脚本不会挂住或改变实例；本地假数据即可复现。

### WP3：opencode 迁移三条样板规则

ENB E5020 编译错误、Get Lost 与显示位置偏好、互斥可选方案。EngineFixes TBB 崩溃仅作为低置信度诊断示例，不自动修改 allocator。

验收：DLL 成功加载但 ENB 编译失败仍能发现；无显示位置偏好时 Get Lost 正常启用；同类可选 mod 能展示冲突但不替用户选择。

### WP4：副总监实现适配器选择与动作执行

统一 GameAdapter、现有 MO2 host 桥接、dry-run/备份/回滚、farm 忙状态与执行前重新验证。跨游戏离线样板通过后，再逐步迁移 Skyrim 常量。

验收：旧 CLI 行为和 Skyrim 实例保持兼容；假游戏不需要 MO2 host 即可探测/映射；配置 overlay 不改原文件；回滚恢复先前状态。

## 本次范围

完成设计与实例地图修复。尚未实施 Lua runtime；opencode 全局服务检查返回 stopped，未创建下游 session，也未切换 provider 或 standalone。
