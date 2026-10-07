# mo-linux 计划

目标：Linux 原生 CLI，复用 MO2 磁盘格式 + 上游 game_bethesda 游戏层，
构造 Skyrim SE 的虚拟 Data（链接农场），再用 Proton 启动。CLI 无状态、幂等、`--json`。

## 架构
- `core/`   纯 C++26，无 Qt：casefold 合并、链接农场、MO2 格式解析、runner。
- `shim/`   Windows API 的 Linux 实现（头文件 + libwinshim），让上游源码**零修改**编译。
- `host/`   `libmo-game`：把 Qt 隔离在此；编译上游 uibase + game_bethesda(gamebryo+skyrimse)，
            提供假 IOrganizer，对外只暴露纯 C++ 接口（std::string / 结构体）。
- `cli/`    子命令 + JSON envelope；只依赖 core 与 host 的纯接口。
- `third_party/` 上游源码，只读，固定 commit。

## 合并规则（casefold merge）
1. 路径每一级以 ASCII 小写为 key 比较。
2. 规范名 = 最先引入该 key 的层的大小写；层 0（游戏本体）最先。
3. 同 key 文件：高优先级层覆盖低层，记录 Conflict。
4. 同 key 在某层是目录、另一层是文件：高优先级层决定类型，产生 Warning。
5. 输出按路径字节序排序；输入层内顺序无关 → 确定性。

## 链接农场
农场根 = 我们独占的启动镜像目录（含 `.mol-farm.json` 标记 + manifest）。
游戏本体作为层 0（prefix 空），各 mod 作为后续层（prefix "Data"），合并后物化为符号链接。
plan = diff(期望, 实际+manifest)；apply 后再 plan 必为空（幂等）。Steam 原游戏目录永不被修改。

## 工作包
| WP | 内容 | 谁 |
|---|---|---|
| WP0 | CMake 骨架、shim、uibase 在 Linux 编过 | 副总监 |
| WP1 | casefold + scan_layer + merge_listings | opencode |
| WP2 | mo2fmt：modlist/plugins/loadorder/Ini/wine_to_unix | opencode |
| WP3 | linkfarm：plan/apply/remove | opencode（依赖 WP1 接口，可并行） |
| WP4 | host：假 IOrganizer + skyrimse 适配 | 副总监 |
| WP5 | CLI + JSON envelope（status/plan/apply/unlink/mods/conflicts/run/doctor） | 副总监 |
| WP6 | Proton/wine runner，plugins.txt/ini/存档同步 | 待定 |

## CLI JSON envelope
`{"schema_version":1,"ok":bool,"command":"...","data":{...},"warnings":[...],"errors":[{"code","message","path"}]}`
stdout 只放结果，日志走 stderr；退出码 0 成功 / 1 一般错误 / 2 用法错误 / 3 检测到漂移（status）。

## 进度事件
长操作（apply / install / run 前的准备）支持 `--events <target>`，输出 JSON Lines（NDJSON），
每行一个事件：`{"event":"progress","op":"apply","done":120,"total":5000,"path":"..."}`，
结束时 `{"event":"done"}`。target：
- `fd:N`    写入已继承的文件描述符（GUI 作为父进程时最简单，推荐）
- `fifo:P`  命名管道（不存在则创建，写端打开会阻塞到有读端；CLI 不负责删除）
- `unix:P`  连接 GUI 监听的 unix domain socket
最终结果仍只走 stdout 的 envelope；事件写失败（对端关闭）不应使命令失败，只静默停止发送。

## 自动诊断与修复
实战中遇到的疑难杂症（ContentCatalog 写坏、VC++ 运行库过旧、ENB 二进制缺失、压缩包布局异常……）的检测/修复计划见 [`PLAN-autofix.md`](PLAN-autofix.md)。
