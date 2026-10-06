# mo-linux 操作指南

面向使用者的「怎么做」手册。命令的完整参数与 JSON 字段见 `mo-linux docs cli`，给 AI Agent 的约定见 `mo-linux docs agent`，设计与已知风险见 `mo-linux docs handbook`。
这份文档在构建时被编进了可执行文件：任何时候 `mo-linux docs guide` 都能看到全文（`| less` 翻页）。

## 1. 它做什么

- **游戏目录永远不被修改**。mo-linux 把「游戏本体 + 启用的 mod + overwrite」合并成一个**符号链接农场**（虚拟游戏目录，默认在实例里的 `farm/`），用 Proton/Wine 从农场启动游戏。
- 实例目录是 MO2 格式（`mods/`、`profiles/`、`downloads/`、`overwrite/`、`ModOrganizer.ini`），可以和 Windows 上的 MO2 互相拷贝。
- 游戏或工具在农场里**新建**的文件，进程退出后收进 `overwrite/`（`Data/` 下的）或 `overwrite-root/`（游戏根目录下的）。
- 游戏或工具**修改/删除/移动**已有文件时走写时复制（COW，见第 5 节），原文件一个字节都不变。

## 2. 安装

构建（需要 GCC 16 / CMake 4 / Ninja；游戏层 host 另需 clang + Qt，见 HANDBOOK §13）：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

把这三个文件放进**同一个目录**（推荐 `~/.local/bin`）：

| 文件 | 作用 | 缺了会怎样 |
|---|---|---|
| `mo-linux` | CLI 本体（文档已内嵌） | — |
| `libmol-cow.so` | 运行游戏/工具时的写时复制钩子 | 运行时只给 warning，退回「改写穿透到原文件」的旧行为；也可用 `MOL_COW_LIB` 指定位置 |
| `libmo-game.so` | 上游 MO2 的游戏插件（读游戏版本、插件列表映射、ini） | `game info`/`skse install`/启动游戏本体不可用；也可用 `MOL_GAME_LIB` 指定 |

依赖的外部程序：`7z`（或 `7zz`/`bsdtar`，解压）、Steam 版 Proton（或系统 Wine）。

## 3. 第一次使用

```sh
mo-linux instance init -i ~/Games/skyrim-mo     # 自动找 Steam 的游戏目录、前缀、最新 Proton
mo-linux instance default --set -i ~/Games/skyrim-mo   # 以后不用每次 -i
mo-linux doctor                                   # 体检：游戏、前缀、Proton、SKSE、插件 master
mo-linux nexus login                              # 粘贴 Nexus 个人 API key（只存在 ~/.config/mo-linux/nexus.key，0600）
mo-linux skse install                             # 按游戏版本装匹配的 SKSE64
mo-linux next                                     # 不知道下一步做什么时就问它
```

`instance init` 探测不到时用 `--game-dir`、`--prefix`、`--proton-path`、`--steam-root` 指定；用系统 Wine 时加 `--runner wine`。

## 4. 日常操作

| 想做的事 | 命令 |
|---|---|
| 装一个下载好的压缩包 | `mo-linux mods install 文件.7z`（带 FOMOD 的要加 `--fomod-defaults` 或 `--fomod 选择.json`，先 `fomod inspect 文件.7z` 看选项） |
| 从 Nexus 装 mod（含前置） | `mo-linux nexus install --mod 12604 --requirements` |
| 搜 mod / 集合 / Wabbajack 列表 | `mo-linux nexus search "skyui"` / `collection search "survival"` / `wabbajack search "lorerim"` |
| 启用、禁用、调顺序 | `mo-linux mods enable 名字` / `mods disable 名字` / `mods move 名字 --to 5` |
| 谁覆盖了谁 | `mo-linux conflicts [--mod 名字]` |
| 插件顺序 | `mo-linux plugins list`、`plugins sort`（只保证 master 在前）、`plugins sort --loot`（近似 LOOT） |
| 把改动落到农场 | `mo-linux apply`（`run` 会自动做；`status` 看农场是否最新） |
| 启动游戏 | `mo-linux run --skse`（阻塞到游戏退出；`--detach` 立即返回；`--dry-run` 只看命令） |
| 看游戏/SKSE/崩溃日志 | `mo-linux logs`、`logs --file skse64.log --tail 200` |
| 查 mod 更新 | `mo-linux mods outdated` |
| 所有输出给程序用 | 任意命令加 `-j`（JSON envelope），进度用 `--events fd:3` |

**免费 Nexus 账号**不能直接下载：先 `mo-linux nxm register`，之后在网页上点「Mod Manager Download」，链接会交给 mo-linux 下到默认实例；集合里等着这个文件的 mod 会被自动标记，再跑一次 `collection install` 即可。

## 5. 集合（Nexus Collections）

```sh
mo-linux collection readme SLUG        # 先读作者的说明：游戏版本、是否要降级、ENB、可选项、装完要跑的工具
mo-linux collection inspect SLUG       # mod 列表、真实下载量、目标游戏版本；不需要实例（search 里的大小是页面声明值，常偏小）
mo-linux collection install SLUG       # 可中断、可重跑；已完成的不重做
mo-linux collection status SLUG        # 已装 / 待处理 / 失败
```

`collection install` 开始前会打印预检：要下多少、`downloads/` 还剩多少空间、游戏版本是否对得上；有 `disk_space` / `game_version` 警告时先停下处理（空间至少留真实下载量的 2 倍）。下载进度事件带速度 `rate` 与剩余时间 `eta`。

`collection install` 从不中途提问。需要你决定的条目记为 **pending**，其他照装，最后退出码 4。逐条处理后再跑一次 `install`：

| pending 类型 | 处理 |
|---|---|
| `fomod_choices` | `fomod inspect` 看选项 → `collection resolve SLUG --mod KEY --fomod 选择.json` 或 `--fomod-defaults` |
| `manual_download` / `nexus_free` | 按给出的 `url` 下载到 `<实例>/downloads/`（文件名随意，按大小 + 哈希匹配），或 `collection resolve … --archive 文件` / `--nxm 链接` |
| `skse` | `mo-linux skse install` |
| 不想要的 | `collection resolve SLUG --mod KEY --skip` |

README 会缓存成 `collections/<slug>/readme-<修订号>.md`，离线也能看。

## 6. 在虚拟目录里运行工具（写时复制，COW）

任何放在农场里的 Windows 程序都可以在合并后的游戏目录里运行，**随便改、删、移、下载文件**：

```sh
mo-linux run --exe "Tools/SomeTool.exe" --args "-a -b"   # 农场相对路径；Data 里的就写 Data/…
mo-linux run --title "SSEEdit"                          # 实例里登记的工具（mo-linux executables list）
```

运行期间：

- 打开已有文件写入 → 先在农场里复制一份再写（**btrfs / xfs 上是 reflink：瞬间完成、不占额外空间**；ext4 上是真复制，大文件要等一下）；
- 删除、改名、移动 → 只动农场里的链接，原文件不动；
- 直接往真实游戏目录、`overwrite/`、`overwrite-root/` 写 → 被拒绝（程序会看到「拒绝访问」）。

退出后（`run` 的 JSON 里 `cow_copies` 是复制了几个文件，`cow_reflinked` 是其中走 reflink 的）：

- 改过的文件收进 `overwrite/`（`Data/…`）或 `overwrite-root/`（游戏根目录，如 exe、dll、ini），**内容没变的副本直接丢弃**；
- `overwrite-root/` 是农场根的最高优先级层，下一次 `apply`/`run` 就生效；
- 删除 / 移动只在那一次运行里有效，下次 `apply` 原样恢复；
- **撤销**：删掉 `overwrite/` 或 `overwrite-root/` 里对应的文件，再 `mo-linux apply`。

不想要 COW 时加 `--no-cow`（工具改写会直接落到原文件！）。报 `wine_busy`：同一个 Wine 前缀里还有一个不带 COW 的 wineserver（比如刚用 protontricks 跑过东西），等它退出或 `wineserver -k` 后重试。

文件系统建议：实例目录与游戏目录在**同一个 btrfs 分区**上时 reflink 才生效（跨分区退化为复制）。ext4 完全可用，只是复制大文件（如 `Skyrim - Textures*.bsa`）时慢、占空间。

### 例：降级游戏（1.7.x → 1.6.1170）

很多集合要求 1.6.1170。用社区的「Downgrade Patcher」不需要动 Steam 的游戏目录：

```sh
mo-linux nexus install --mod <补丁的 mod id>   # 或 mods install 下载好的压缩包；含 exe 的会装成根目录型 mod
mo-linux apply
mo-linux run --exe "<补丁程序>.exe"            # 补丁读原文件、写新文件，全部被 COW 接住
mo-linux game info                              # version 应变成 1.6.1170
mo-linux doctor
```

补丁改出的 `SkyrimSE.exe`、`SkyrimSELauncher.exe`、`Data/Skyrim - Shaders.bsa` 进了 `overwrite-root/` / `overwrite/`，Steam 目录保持 1.7（Steam 再更新也不影响农场）。之后 `skse install` 会按新版本选 SKSE。恢复 1.7：删掉 `overwrite-root/` 里的那几个文件和 `overwrite/` 里的 bsa，`apply`。

限制：32 位程序拿不到 64 位钩子（没有 COW）；装在农场**之外**的工具（`executables list` 里 `farm_path` 为空的）看不到虚拟的 `Data/`。

## 7. 排错

| 现象 | 做法 |
|---|---|
| 不知道哪里不对 | `mo-linux doctor`，`mo-linux next` |
| `farm_busy` | 游戏或工具还在用农场，先退出它 |
| 插件没加载 | `plugins list` 看 `issues`（缺 master / 被禁用 / 顺序错），`plugins sort` |
| SKSE 没生效 | `logs --file skse64.log`；`doctor` 的 `skse.version` 是否匹配游戏版本 |
| 游戏崩溃 | `logs` 里找 crash 日志（需要装崩溃日志 mod） |
| 农场被弄乱了 | `mo-linux unlink && mo-linux apply`（只删链接，不碰源文件） |
| 想让 Steam 直接启动也能看到某些文件 | `overwrite promote --filter 'Data/*.esl'` 先预览，确认后加 `--yes`（**唯一**会写真实游戏目录的命令） |

## 8. 图形界面与 AI

- 图形界面：Linux Cockpit 的 `modorg` 能力（仓库 launcher-modorg）。它只调用 `mo-linux -j`，和命令行完全一致。
- AI Agent：先 `mo-linux docs agent`，再 `mo-linux -j schema`；`mo-linux -j next` 给出可直接执行的下一步。标了 `confirm` 的命令（启动游戏/工具、`overwrite promote`）要先问用户。

## 9. 文档

```sh
mo-linux docs            # 列出内嵌的文档
mo-linux docs guide      # 本文
mo-linux docs agent      # 给 AI 的协议
mo-linux docs cli        # 完整命令规格
mo-linux docs handbook   # 设计、验证状态、风险
mo-linux -j docs cli     # JSON：data.markdown 是全文
```
