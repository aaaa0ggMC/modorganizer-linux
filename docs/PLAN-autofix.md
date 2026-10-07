# 计划：把「疑难杂症」做成 mo-linux 的自动检查与修复

> 来源：2026-10-07 Constellations（2444 mods，1.6.1170 降级）实战，从「clean 启动闪退」到「194 个 SKSE 插件全部加载」一路排查的痕迹。
> 目标：下次同样的问题，`doctor` / `next` 直接说出原因并给出（或自动执行）修复，人和 AI 都不用再翻 PROTON_LOG。
> 记录：`docs/SESSION-2026-10-06-constellations.md` §九–§十一。

原则（沿用）：
- **检测进 `doctor`（带稳定 id），修复进 `next` 的 fix 命令**；会改 Steam 前缀/游戏目录的修复必须显式命令触发，`next` 里标 `[confirm first]`。
- 每条检测都要能**离线、只读、秒级**跑完（不启动游戏）。
- 每个修复要有 **备份 + 可回滚**（改前缀文件 → 留 `.mol-bak`）。

---

## 已在本轮修复（代码已合入工作区，未提交）

| # | 现象 | 根因 | 修复 | 测试 |
|---|---|---|---|---|
| A17 | 缺 master（`Requiem - Immersive College of Winterhold.esp`）、多出不该有的补丁（`JKJ - AIO Patch.esp`） | 清单 `hashes`（Vortex「复刻」= list installer，**只**装列出的文件）未实现，被当成「无选择 FOMOD」走默认 | `FomodMode::Replicate`：按 path+md5 挑文件（同路径优先，再全包按 md5 找，硬链接/复制）；`mol_fomod=replicate:<xxh64>`，旧安装自动原地重装 | `manifest_hashes_replicate_the_curators_files_and_fix_old_installs` |
| A18 | `Skyrim Unbound Addon - Bruma.esp` 缺失；TiE 补丁装成了错误版本 | 清单选中的插件在我们这边算成 `NotUsable`（它的 `fileDependency` 指向的 mod 按安装顺序还没装） | 宽松模式下有文件的照装；`fileDependency` 把清单插件列表（启用）视作 Active | `manifest_choice_of_a_not_yet_usable_plugin_is_honoured` |
| A19 | 87 个 FAIL：`requires 3DNPC.esp` | `Data/` + `Patch Notes.txt` 的压缩包没剥 `Data`，内容躺在 `mods/X/Data/…` 不生效（以前被主 mod 自带的旧 esp 掩盖） | 顶层有 Data、其余不是游戏数据 → Data 为根 | `data_folder_next_to_readme_files_is_the_mod_root` |
| A20 | Lux Patchhub 原样全装、清单选择全没生效 | 整包放在 `Fomod/` 下被当「单层包装」剥掉，认不出 FOMOD | 永不剥 `fomod` 目录 | `archive_living_entirely_inside_fomod_is_still_a_fomod` |
| A21 | farm 根出现文件名 `Data\SKSE\Plugins\PriorityMod.dll` | Windows 打的 zip 用 `\` 作分隔符，Linux 解压当文件名 | 解压后把名字里的 `\` 拆成目录（越界拒绝） | `backslash_paths_from_windows_zips_become_directories` |
| A22 | `DynDOLOD.esp`/`Synthesis.esp`/`Constellations - Compatibility 3.esp` 等**末尾的插件不在 plugins.txt 里**，另有 34 行重复 | 清单 `plugins` 有 35 个重复名，`apply_plugin_spec` 不去重：重复项各占一个槽位，把末尾插件挤出列表 | 按 casefold 只认第一次出现 | `collection_plugin_spec_with_duplicates_keeps_every_plugin` |
| A23 | 进主菜单弹 `Dismembering Framework.esm is missing`；游戏把 profile 的 plugins.txt 重写成**全部无 `*`**（文件头变成 `# This file is used by Skyrim…`） | 原版运行留下真实的 `AppData/…/Plugins.txt`（大写 P），`plugins sync` 按上游映射建小写 `plugins.txt` 链接——Linux 上两者并存，**Wine 优先打开大小写完全一致的 `Plugins.txt`**（原版空列表）→ 一个 mod 插件都没加载，游戏再按 loadorder 把列表写回（全禁用） | 建链接前把只差大小写的同名条目改名 `.mol-backup`（链接直接删） | `case_variant_shadows_of_the_target_are_backed_up` |
| A24 | 合集的 ENB 预设（Constellations - ENB Preset 等 6 个）落在 `Data/enbseries`，ENB 读不到 | 清单 `details.type`（Vortex mod 类型 `enb`/`dinput` = 部署到游戏根目录）被忽略 | `enb`/`dinput` 装成根目录型；已装的在 `collection install` 时补根目录标记（不重装） | `vortex_enb_and_dinput_mods_deploy_to_the_game_folder` |
| — | 修了安装逻辑但旧 mod 已装错 | 没有「重装」入口 | `collection resolve --mod KEY --reinstall`（新内容齐了才替换旧目录，modlist 位置不变） | 同 A17 |

### 本轮修复的后续（P0）
- **P0-1 自动发现「按旧逻辑装错」的 mod**：本轮靠手写 Python 扫描找出 A19/A20/A21 的受害者（顶层 `Data/`+readme、顶层有 `ModuleConfig.xml`、文件名含 `\`）。应做成 `doctor` 检查 `mods.layout`（只读扫描 mod 顶层，秒级），`next` 给出 `collection resolve --reinstall` / 通用 `mods reinstall NAME`（非集合装的 mod 也要能重装，需要记录来源压缩包）。
- **P0-2 A18 的已装受害者无法自动识别**：选择指纹没变，旧安装不会自动重装。方案：`mol_fomod` 增加解析器版本号（`choices:v2:<hash>`）；`collection install --verify-fomod` 只解 fomod 目录、用新逻辑重算文件清单并与 mod 目录比对，不同才重装（避免 359 个 FOMOD 全量重装）。
- **P0-3 `hashes` 不完整时的诊断**：本轮 `Interesting NPCs 3DNPC` 的 hashes 不含 `3DNPC.esp`（由 Hotfix 提供）。复刻后 `doctor` 的缺 master 检查已能发现；再加一条：缺 master 时在**所有已装压缩包的文件清单**里找提供者，给出「哪个 mod 的压缩包里有、为什么没装上」。

---

## 第二轮（2026-10-07 下午）已实现
- **D1** `game.content_catalog` + `fix content-catalog`（改名备份）。
- **D2** `prefix.vcrun`（读 PE 的 VS_FIXEDFILEINFO，不靠 strings）+ `fix vcrun`（protontricks 优先，否则自己下载 vc_redist 用实例 Proton 静默装，装完复查）。
- **D3** `enb.binaries`（ENB 预设/`ENBHelperSE`/`KiENBExtender`，含 `KiLoader/Plugins`）+ `enb install --archive`（装成根目录型 mod，不碰游戏目录）。
- **D4/P0-1** `mods.layout.{nested_data,raw_fomod,backslash_name,enb_in_data}`（只看 mod 顶层；2444 个 mod 秒级）。
- **D4b** `prefix.case_shadows`（修复复用 `plugins sync` 的影子备份）。
- **D7（部分）** `skse.plugins`：读上次运行的 `skse64.log`，失败插件计数与名单；日志早于运行库更新时不报（修复后不再误报）。
- **D9（新）Steam 客户端没在运行**：实测 2026-10-07 13:31，Steam 未启动时 `run --skse` → `SteamAPI_Init() failed` → 32 位 `steam.exe steam://run/489830` 请 Steam 拉起 **Steam 库里的原版**（不经过 farm/COW，会再次写坏 ContentCatalog）。`run` 现在先查本机 `ubuntu12_32/steam` 进程，没有就 `prefix_unhealthy`。另：`ERROR: ld.so: … libmol-cow.so … wrong ELF class: ELFCLASS64` 是 32 位 `steam.exe` 打的，**无害**（64 位 wineserver/游戏照常挂载 COW，见 `/proc/<pid>/maps`）。待做：`run --start-steam` 自动 `steam -silent` 并等登录。
- **D10 `terminate`**：结束占用实例的全部进程（农场 cwd/命令行 + 前缀环境变量），SIGTERM→SIGKILL→`wineserver -k`；`farm_busy`/`wine_busy` 报错与 `doctor farm.busy` 都指向它。实例：Steam 未启动时留下的 `steam.exe steam://run/489830`（cwd 在农场）让 `run` 一直 `farm_busy`；以及 wineserver 死后残留的一整套孤儿 `services.exe/winedevice.exe/explorer.exe`。测试 `tests/test_terminate.cpp`（真子进程：农场 cwd、前缀环境、无关进程、shell、忽略 SIGTERM 的进程）。
- **D8** `run` 启动前门：运行库过旧 / ContentCatalog 写坏 → `prefix_unhealthy` 拒绝启动，`--force` 跳过。
- 测试：`tests/test_health.cpp`（7 项）。
- 未做：D4c（游戏改写 plugins.txt 的检测，需按「去掉隐式插件后比较集合」）、D4d（KreatE 等解析符号链接真实路径的插件 → 硬链接规则）、D5（降级 depot 校验 / `game downgrade`）、D6（混合显卡）、D7 的 `run --diagnose`（PROTON_LOG 自动解析）、P0-2、P0-3。

## 待做：新的 doctor 检查 + 修复

### D1 `game.content_catalog`：坏掉的 ContentCatalog.txt（clean 启动 ~8s 闪退）
- **现象**：原版 1.6.1170 启动后约 8 秒退出，无弹窗；PROTON_LOG 里线程抛 `e06d7363`（MSVCP140 的 C++ 异常）后进程结束。
- **根因**：用 Steam 跑过 1.7.x（例如为了下载 AE/Creations）后，`<prefix>/drive_c/users/steamuser/AppData/Local/Skyrim Special Edition/ContentCatalog.txt` 被改写，含 `"Version" : "1701307962.== Version Number =="`，1.6.x 解析版本号抛 `std::invalid_argument`。
- **检测**（只读）：游戏版本 < 1.7 且 ContentCatalog.txt 中任一 `Version` 不匹配 `^\d+(\.\d+)?$` → error。
- **修复**：`mo-linux fix content-catalog` → 改名为 `ContentCatalog.txt.mol-bak-<时间>`（游戏会重建）。`run` 前自动检测，命中时提示（不静默改）。
- **注意**：每次用 Steam 启动 1.7.x 都会再写坏一次 → `run` 前每次都查。

### D2 `prefix.vcrun`：VC++ 运行库过旧（大批 SKSE 插件 "fatal error occurred while loading plugin" / `couldn't load plugin (000003E6)`）
- **现象**：SKSE 弹窗列出几十个插件（CrashLogger、EngineFixes 系、TrueHUD…）加载失败；插件自己的日志一行都没写。
- **根因**：前缀 `system32/msvcp140.dll` 是 **14.00.24215**（VC++ 2015，Steam 首次运行脚本 / 游戏自带 `_CommonRedist` 装的）。VS2022 17.10+ 编译的 CommonLibSSE-NG 插件需要 **≥ 14.40**（`std::mutex` constexpr 构造的 ABI 变化：旧运行库上一加锁就崩）。
- **检测**（只读）：读 `system32/msvcp140.dll`、`vcruntime140.dll`、`vcruntime140_1.dll` 的 VERSIONINFO（PE 资源，不用 strings）；< 14.40 → error；`vcruntime140_1.dll` 是 Wine builtin 存根 → warn。另查注册表 DllOverrides 是否为 `native,builtin`。
- **修复**：`mo-linux fix vcrun`：
  1. 有 `protontricks`：`protontricks <appid> -q vcrun2022`（注意：`-q` 必须放在 appid **之后**，放前面会报 unrecognized arguments）；
  2. 否则内置：下载 `https://aka.ms/vs/17/release/vc_redist.x64.exe` → `proton run vc_redist.x64.exe /quiet /norestart`（STEAM_COMPAT_* 环境与 `run` 一致）→ 写 DllOverrides。
  3. 改前先备份三个 DLL。
- **何时会被打回旧版**：Steam 验证文件 / 首次运行脚本 / Proton 重建前缀（日志 `Proton: Upgrading prefix from None to …`）。`run` 前每次检查。

### D3 `enb.binaries`：有依赖 ENB 的插件但没有 ENB 二进制
- **现象**：KiLoader 弹窗 `ENBHelperSE.dll … Needs [Default, ENB] loading point(s)`、`KiENBExtender.dll - Import dependency missing from: d3d11.dll`；点 No 能继续，但 ENB 效果全无。
- **根因**：合集只带 ENB 预设（enbseries/、enblocal.ini），ENB 本体（`d3d11.dll`、`d3dcompiler_46e.dll`）enbdev.com 不允许再分发，只能浏览器手动下。
- **检测**：farm 根（游戏层 + 根目录型 mod）有 `enblocal.ini`/`enbseries` 或 `SKSE/Plugins` 里有 `ENBHelperSE.dll`/`KiENBExtender.dll`，但根没有 `d3d11.dll` → warn（needs_human）。
- **修复**：`mo-linux enb install --archive <enbseries_skyrimse_v*.zip>`：只取 `WrapperVersion/d3d11.dll`、`d3dcompiler_46e.dll`，装成根目录型 mod `ENB Binaries (vX)`（不碰游戏目录）。`next` 给出下载页 URL 与合集 readme 要求的版本。
- **Proton**：ENB 的 d3d11.dll 需要 `WINEDLLOVERRIDES=d3d11=n,b`（DXVK 由 ENB 内部转调）——`run` 检测到 ENB mod 时自动加；并需验证与 DXVK 的叠加方式（ENB 的 `[PROXY]` 链到 dxvk 的 d3d11）。

### D4 `farm.names`：文件名含 `\` / 顶层异常布局（A19–A21 的事后检测）
- 见 P0-1。另：farm 根出现含 `\` 的名字 → error（一定是装坏了）。

### D4b `prefix.case_shadows`：前缀里只差大小写的同名文件
- A23 的事后检测：`AppData/Local/Skyrim Special Edition/` 与 `Documents/My Games/Skyrim Special Edition/` 下，对每个映射目标找 casefold 相同但名字不同的条目 → error（游戏会读错文件）。修复 = `plugins sync`（已自动备份）。
- 推广：任何「我们建链接、Wine 程序按另一种大小写打开」的位置都有此风险（ini、Saves、SKSE 日志目录）。

### D4c `plugins.game_rewrote`：游戏改写了 plugins.txt
- 检测：profile `plugins.txt` 首行不是 mo-linux/MO2 写的头（`# This file is used by Skyrim…`）且 `*` 行数骤降 → error，提示「游戏读到的不是我们的列表」，自动查 D4b。
- 修复：`collection install`（重新套用清单）或 `plugins` 从备份恢复。**`plugins sync`/`run` 前给 profile 的 plugins.txt 留一份 `.mol-last-good`**，游戏改坏后可一键回滚。

### D4d `farm.symlink_hostile`：会解析符号链接真实路径的插件
- 实例：**KreatE** 启动时备份预设，按 farm 链接的**真实目标**算相对路径，得出 `Data\KreatE\Presets\DALC Fix\Backup_1.2.0\..\..\..\..\mods\DALC Fix Preset\…` → `copy_file: Path not found`，红字「Unable to load one or more config(s)」（只影响该预设，不崩）。
- 方案：farm 支持「物化方式」按路径规则选择：默认 symlink；命中规则的用**硬链接**（farm 与 mods 同一文件系统时；否则 reflink/复制）。内置规则表起步：`Data/KreatE/**`，以后遇到同类插件追加。COW 照常（写硬链接前先断开）。
- 检测：`run --diagnose` 抓到含 `..\..\` + `mods\` 的报错时，提示加规则。

### D4e 注意：游戏重写 plugins.txt 的两种情况
- **正常**：首行 `# This file is used by Skyrim…`，`*` 行数 = 我们的启用数 − 隐式加载的本体/DLC/CC（本轮 2449 → 2369，少的 80 行全是 `Skyrim.esm`/CC）。D4c 的检测必须按「去掉隐式插件后比较集合」判断，不能只看文件头或行数。
- **异常**（A23）：`*` 全没了。

### D5 `game.depot_mix`：降级残留 / 混装
- 本轮复制了 1.7.104 的 148 个 `cc*` 到 1.6.1170 副本，结果可用，但风险点：`_ResourcePack.esl/bsa`、`Skyrim.ccc` 来自不同版本。
- **检测**：`SkyrimSE.exe` 版本 vs `Skyrim.ccc`/`_ResourcePack` 时间戳与大小的已知表（1.6.1170 / 1.6.640 / 1.5.97 depot manifest 的文件 md5 清单）→ 不一致 warn。
- **修复**：`mo-linux game downgrade`：调 SteamCMD `download_depot 489830 489831/489832/489833 <manifest>`（人输密码 / Steam Guard，凭据不落盘不打印），合并到独立副本目录；记录 manifest id；自动跑 D1。

### D6 `gpu.prime`：混合显卡（待验证）
- 本轮 DXVK 已落在 RTX 5060（PROTON_LOG 中 `NVIDIA GeForce RTX 5060 Laptop GPU`），暂无问题。`doctor` 读 DXVK 日志的设备名，落在核显时 warn，修复为 `run` 加 `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia DXVK_FILTER_DEVICE_NAME=…`。

### D7 `run` 的启动诊断（把本轮手工排查变成命令）
本轮手工步骤：`PROTON_LOG=1 WINEDEBUG=+seh,+loaddll` 启动 → 找 `dispatch_exception code=` → 用 `info[3]`（抛出模块基址）对 `Loaded … at <基址>` 找到模块 → 结论。做成：
- `mo-linux run --diagnose [--vanilla]`：带上述日志启动（`--vanilla` = 不经 farm/SKSE 直接跑游戏目录，即 clean 启动），进程退出后自动解析：
  - 未处理异常码 + 抛出模块（`MSVCP140` → 先查 D1/D2）；
  - `skse64.log` 里 `disabled, fatal error` / `couldn't load plugin (XXXXXXXX)` / `reported as incompatible` 的插件清单与计数（> 10 个 fatal → 几乎必是 D2）；
  - 新的 CrashLogger `crash-*.log` 摘要（顶部模块 + 可能的插件）。
- 输出写进实例的 `logs/run-<时间>.json`，`doctor` 读最近一次结果。
- **注意**：`pkill -f SkyrimSE` 会误杀自己的 shell（命令行里含该字符串）；`run` 应记录子进程 pid，提供 `mo-linux run --kill`。

### D8 `run` 前置门（汇总）
`run --skse` 前按顺序快速检查：D1 → D2 → farm 同步 → plugins 链接 → D3（warn 不拦）。有 error 时拒绝启动并打印 `next`，`--force` 跳过。

---

## 排查时用到的事实（供实现参考）
- SKSE 日志：`<prefix>/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/skse64.log`；成功的插件行含 `loaded correctly`（本轮修复后 194 个，修复前 136 个）。
- `000003E6` = `ERROR_NOACCESS`（DLL 初始化时访问违例），配合 msvcp140 < 14.40 出现。
- Address Library 在 farm 里是 `Data/skse/plugins/versionlib-1-6-1170-0.bin`（小写目录无碍：Wine 大小写不敏感）。
- Proton `version` 文件缺失会触发 `Upgrading prefix from None`，可能伴随 DLL 回退 → D2 要在此后复查。
- 1SpriggansSE（`meshes/Meshes`）、Skyland Complex Material Update（`textures/Textures`）的 merge 警告：压缩包自带的大小写重名目录，farm 按 casefold 合并，文件不丢——`doctor` 可把「层内 casefold 冲突但无文件丢失」降级为 info。
