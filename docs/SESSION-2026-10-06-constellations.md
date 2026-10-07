# 实战记录：Constellations 合集安装（2026-10-06）

> 真实环境端到端安装 [Constellations - A true RPG](https://www.nexusmods.com/games/skyrimspecialedition/collections/9zfscf)（rev 121，2444 mods）时观察到的**异常 / 缺陷 / 使用体验**。
> 状态：**进行中**，随安装推进持续追加。归集者：本次会话。

## 环境

| 项 | 值 |
|---|---|
| mo-linux | `0.0.1`（`main` b905ae3 后重编，GCC 16 + clang 22 子工程） |
| 实例 | `/mnt/skyrim/Constellations`（btrfs，USB 3.2 Gen2 SSD，477G） |
| 游戏 | Steam `Skyrim Special Edition`，**1.7.104.0**（合集要求 1.6.1170.0） |
| 前缀 / Proton | `compatdata/489830/pfx` / Proton 9.0 (Beta) |
| Nexus | Premium / Supporter |
| 网络 | 经 `FlClash` 代理接口（TUN），下载 ~11 MB/s |
| 硬件 | 32 线程 / 30 GiB / RTX 5060 Laptop 8GB（nvidia 615）+ AMD 610M iGPU |
| 磁盘 IO（该 USB 盘，实测） | 顺序读 760 / 写 778 MB/s；随机 4K QD32 读 ~6.1k IOPS、写 ~2.6k IOPS |

---

## 一、异常 / 缺陷

### A1. 【数据】`collection inspect`/`search` 报告的 `total_size` 比实际下载量少 ~18% — **较严重**

- **现象**：GraphQL `latestPublishedRevision.totalSize` = **126,527,590,575 B ≈ 126.5 GB**，而清单里所有 `source.fileSize` 之和 = **149,830,882,130 B ≈ 149.83 GB**。二者相差 **23.3 GB（+18.4%）**。
- **证据**：
  - `collection inspect` 输出 `total_size` 取自 `NexusCollectionRev.total_size`（`core/src/collection.cpp` → `cli/cmd_collection.cpp:208`），即 Nexus 的 `totalSize`。
  - 实际 `collection install` 进度事件 `total` = **149830882130**（用清单求和），与 `sum(source.fileSize)` 完全一致。
- **影响**：用户按 inspect/search 显示的 126.5 GB 规划磁盘（作者也按 375 GB 建议），实际要多下 23 GB；下载 ETA、剩余空间判断都会偏差。对「单盘 375G」这种卡边界的场景是实打实的坑。
- **建议**：`inspect`/`search` 在拿到清单后，`total_size` 用清单 `sum(fileSize)`（或同时输出 `declared_size` 与 `manifest_size` 两个字段）；`search` 只拿得到 Nexus 值时可标注为「declared / 可能偏低」。

### A2. 【UX】只想「看看要求」也必须先有实例

- **现象**：`collection inspect`（评估能不能装、多大、有没有 unsupported）需要实例（`load_instance`）；而 `collection readme`、`nexus search`、`collection search` 不需要。
- **影响**：本次为了在「还没建实例、盘还没定」的阶段评估合集，只能在 `/tmp` 建了个一次性实例。对「先看看要求再决定装不装」的决策流程是纯摩擦。
- **建议**：`collection inspect` 对无实例的情况降级到 `search_domain`（像 `readme` 那样），只做「拉清单 + 统计」，`status` 一律为 `new`。

### A3. 【UX】`nexus whoami` 文本输出不显示 Premium

- **现象**：默认文本输出只有 `nexus whoami: ok`；`is_premium`/`is_supporter`/`name` 只在 `--json` 里。而合集能否自动下载**关键取决于 Premium**。
- **建议**：文本输出补一行账号与 Premium 状态（如 `128974369 (premium)`）。

### A4. 【UX】下载进度事件没有速率 / ETA

- **现象**：`--events` 只有 `{"op":"download","done":N,"total":M}` 与 `{"op":"downloaded",...,"item":...}`；没有 bytes/s、没有 ETA、没有「第几个文件 / 共几个」的下载序号（`downloaded` 的 `done/total` 是文件计数，但和字节进度是两套）。
- **影响**：GUI/脚本要自己算速度和 ETA；本次监控只能靠采样目录大小。对一个要跑 3–4 小时的下载，是体验短板。
- **建议**：`download` 事件带上 `rate`/`eta`；或在 `downloaded` 里带上当前文件名（已有 `item`）与累计字节。

### A5. 【体验】`--events fd:1` 与最终 envelope 同流

- **现象**：把 events 指到 stdout 时，NDJSON 进度与最后的 JSON envelope 混在一个流里，需要靠「最后一行」区分。
- **影响**：本次后台日志因此不便直接 jq。属文档/用法问题（events 本该走独立 fd），但值得在 CLI.md 里把「何时用 fd:N vs fifo」讲得更直白。

### A6. 【数据/UX】续跑后进度事件 `total` 语义突变

- **现象**：首次运行时 `{"op":"download","done":0,"total":149830882130}`；中断后重跑，变成 `total:146141821749`（= 149.83GB − 已完成的 ~3.69GB），`done` 也从 0 重新起算。
- **影响**：同一个 `op` 的 `total` 跨进程不一致——首次是「全量」、续跑是「剩余」。GUI/脚本若想画「总进度条」或算 ETA，会把总量算少；若按两次日志拼接，数字对不上。
- **建议**：`total` 恒为全量下载量，额外给一个 `skipped`/`already_have` 字段；或统一 `total = 已完成 + 剩余`。

---

## 二、正面体验（保持）

- **`instance init` 全自动探测**：game dir、Wine 前缀、最新 Proton、profile 一次到位，零参数（除 `-i`）。
- **`collection readme` 免实例**：能在建实例前读到作者完整 README（含系统要求、AE/Downpatch 警告），决策信息很全。
- **`collection inspect` 能提前判定可装性**：本次提前得出「2444 全 Nexus、0 二进制补丁、0 外部来源、386 个 FOMOD 都有选择」→ 结论「mo-linux 侧可全自动装」，非常有价值。
- **清单求和口径正确**：`install` 进度 `total` 用清单 `fileSize` 求和，与真实下载量吻合（问题只在 inspect 展示的口径，见 A1）。

---

## 三、待观察（安装/运行阶段，后续追加）

- [ ] 386 个 FOMOD 的自动选择是否全部命中（对比 patch 数量）
- [ ] `optional`（27 个）默认是否安装，是否符合预期
- [ ] 游戏版本 1.7.104.0 ≠ 1.6.1170：`notes` 是否出现、`doctor` 提示是否清晰
- [ ] SKSE pending（silverlock 外部源）→ `skse install` 流程
- [ ] 2444 mod 的 `apply`（符号链接农场）耗时与正确性
- [ ] 插件顺序 2418 个 / 2689 条规则的应用结果
- [ ] 混合显卡（AMD iGPU + NVIDIA）下 Proton 实际用哪块 GPU、是否需要强制 PRIME offload
- [ ] ENB / DLAA 在 Proton 下的实际表现
- [ ] 下载 149.8 GB 实际耗时与代理稳定性

---

## 四、多合集复用与磁盘规划（2026-10-06 追问）

**（正向）复用确实存在，但限于「同一个实例内」**：
- **下载归档**：按「大小 + md5」复用（`core/src/nexus.cpp:579 out.reused`）。已下过的包不再重下。
- **已装 mod**：按 `meta.ini` 的 `(modid, fileid)` 复用，`core/src/collection.cpp:540` 注释明确写「另一个集合、`nexus install` 或 MO2 装的」都算，跳过重复安装。
- 结论：**多个合集装进同一个实例（不同 `-p PROFILE`）**时，重复 mod 只占一份、重复下载只下一份。正确姿势是一个实例多 profile，而不是「一个合集一个实例目录」。

**（风险 R-a，待验证）复用键不含 FOMOD 选择**：实例里 `mods/` 是各 profile 共享的，复用键只有 `(modid, fileid)`。若合集 A 与合集 B 用同一 mod+file 但**选了不同 FOMOD 选项**，B 很可能直接复用 A 的安装结果 → 选项串味。需要实测或在复用判断里纳入选择指纹。

**（UX）跨实例不自动复用**：每个实例各有 `downloads/`、`mods/`；想在两个实例间共享下载，只能手动 symlink `downloads/`。`collection install` 也没有 `--downloads`（Wabbajack 有）。

**磁盘规划**：477G 盘装一个 Constellations（含下载 ~315–345G）后余 ~130–160G。两个大列表的并集取决于重叠度（modlist 之间重叠通常很高），但两个独立超大 RPG 列表可能放不下；把 `downloads/` 挪到内置 NVMe（`/home` 尚余 519G）可给 USB 盘腾出 ~150G。

---

## 五、设计构想（用户提出）：共享 mods sink（解压内容复用库）

**构想**：解压后的 mod 内容放到一个共享库（sink），例如 `sink/<modid>/<fileid>/`（或 `mods/XXX/{version}/`），各实例的 `mods/<name>` 用符号链接指过去，启动时统一从 sink 出。Linux 下用 symlink 天然可行。

**为什么契合**：mo-linux 的架构本来就是「mods 真实文件 → symlink farm（合并视图）」；再加一层「实例 mods → sink」只是在存储层多一级 symlink。COW（libmol-cow）也天然友好——游戏写的是 farm 里的链接，COW 复制到 overwrite，sink 永远不被改。

**收益**：跨实例去重（现在只有实例内按 `(modid,fileid)` 复用）；同一 mod 多版本共存/回滚；切换/新建实例近乎零拷贝。

**三个真难点**：
1. **键与 FOMOD**：键若只用 `(modid,fileid)`，不同 FOMOD 选择会串味（同 R-a）。要么键里加选择指纹 `(modid,fileid,fomod-hash)`，要么 sink 只存「原样解压树」，把选择留到 farm 构建期应用（更正确，但改动大）。
2. **引用计数 / GC**：sink 条目被多个实例的 symlink 引用，删除某个实例后要到「无人引用」才能回收，需要一个跨实例扫描的 GC（或手动）。
3. **symlink 感知**：farm 扫描（`scan_layer`）、`build_farm_model` 的层根、`remove_farm` 的非递归保护、`meta.ini`/`list_mods` 读取，都要确认能正确穿过「mod 目录本身是 symlink」这一层；`instance.cpp` 的 mod 目录若指向 sink，需保证 MO2 兼容读写不写坏 sink。

**建议的折中**：先做**带 `mods_store` 配置的 Nexus 专用 sink**——安装时解压进 `store/(modid,fileid[,fomod-fingerprint])`，实例 `mods/<name>` 建成 symlink；只读、不 GC（手动清理）。拿到 90% 收益、风险可控。

**零改动备选**：单实例多 profile（现有能力）已覆盖同盘多合集；sink 的边际价值主要在「跨实例 / 跨盘」复用。

---

## 六、新版本 `69b35e7`（PR #3）回归观察 — 2026-10-06 晚

拉取 `1e979f4 → 69b35e7`（commit `7ef9c24 collection: real download size, stable progress totals, rate/eta, preflight`）并重编，重启合集安装后观察：

**已修复确认**
- **A1 ✅**：preflight 用清单求和，`download=149830882130`（真实量），不再是 Nexus 的 126.5GB。
- **A6 ✅**：`total` 恒为全量；`done` 从 `have`（已下/已装）起步 → 首次与续跑一致。代码见 `core/src/collection.cpp:450-474`。
- **P5 ✅**：preflight 落地，开始下载前打印 `mods/size/free` 并解析 `game_version`：
  `collection install: 2444 mods, 139.54 GB to download (0 B already here), 437.34 GB free in downloads/`
  `warning [game_version] the collection targets game version 1.6.1170.0 but this game is 1.7.104.0`

**新发现的小瑕疵**
- **A7（UX）preflight 的 “already here” 只读 `state.json`，不扫 `downloads/`**：本次已有 38GB 下载在盘上，却显示 `0 B already here`、`remaining=全量`。真正下载时会用 `find_cached()`（大小+md5）复用、不会重下，但**预检数字对用户是误导**。建议 preflight 也做一次按大小的快速扫描（md5 可省）。代码：`cli/cmd_collection.cpp:222-237`（`have` 只看 `st.mods`）。
- **A8（文案）`human_size` 把 GiB 标成 “GB”**：`139.54 GB` 实为 149.83e9 B = 139.54 GiB。数字对、单位标签不对，建议改 “GiB” 或按 10^9 显示。

**待确认**
- A4（rate/eta）：事件字段已加入（commit 描述），等出现 `download` 进度行后核对实际是否带 `rate`/`eta`。

**A4 复核（已确认，但有问题）** — 实测 `69b35e7`：
- 好处：能算出来。实测 `"rate":16642725`（≈16.6 MB/s）+ `"eta":6549`，与外部采样（15.5 MB/s）一致。
- **问题 A9（UX/数据）`rate`/`eta` 只在少数事件上出现**：`install3.log` 里 598 条 `op=download` 事件，仅 **77 条**带 `rate`；其余没有。`cli/events.cpp:274` 仅在 `rate > 0` 时附加，而 `rate_of()`（`:226-233`）要求样本窗口 ≥1s 且有增量，否则返回 0 → 大量事件缺字段，消费方不能假设字段存在。
- **问题 A10（数据/单位）`downloaded`（计数型 op）的 `rate`/`eta` 语义错**：`downloaded` 的 `done/total` 是**文件个数**，但 `rate_of` 按同一套算「个/秒」，数值很小（如 0.3），`emit_line` 里 `static_cast<int64_t>(rate+0.5)` 截断成 `rate:0`，而 `eta` 仍用未截断的 0.3 算 → 出现 `"rate":0,...,"eta":32984` 这种自相矛盾的字段。且不同 op 的 `rate` 单位不同（`download` 是 B/s、`downloaded` 是个/s），字段名没区分。
- **建议**：缓存最近一个非零速率并持续附带；`eta` 仅在 `rate` 截断后 >0 时才给；计数型 op 改 `items_per_sec` 或干脆不给 `rate`；文档里标明每个 op 的 `rate` 单位。

**A11（UX/正确性）`next`/`doctor` 不知道合集要求的游戏版本，会给出与合集矛盾的 `skse install`**
- 现象：合集目标 `1.6.1170.0`，本机游戏 `1.7.104.0`。`collection install` 的 preflight 会警告版本不匹配；但 `next` 只给出：
  - `skse.version`（blocking）「SKSE64 does not match the game: skse64_1_7_104.dll not found」→ 建议 `skse install`
  - `farm` → `apply`
- 问题：`next`/`doctor` 只针对**当前游戏版本**（1.7.104）算 SKSE。若照它执行 `skse install`，会装 **1.7.104 的 SKSE（2.3.1）**，而这个合集要 1.6.1170（应装 SKSE 2.2.6）——**指向了错误的一步**，且完全没提“先降级游戏”。
- 影响：把 `next` 当唯一指引的 agent/用户会走错路；「一键启动」的目标因此不可达。
- **建议**：`next`/`doctor` 读取未完成集合的 `gameVersions`（preflight 已有数据），当与当前游戏版本不符时，插入一条 **needs_human** 的 blocking 步骤（如 `downpatch_game`，附 `collection readme`/Downpatcher 链接），并把 SKSE 步骤指向**目标版本**而非当前版本。

**理想「一键启动」还差的外部步骤（本轮实测清单）**
1. 游戏降级 `1.7.104.0 → 1.6.1170`（Steam depot 降级 / 作者 Downpatcher；mo-linux 不管，需人做）
2. 补齐 AE：需 148 个 `cc*`，当前 140
3. `skse install`（必须在降级**之后**做，才会装 1.6.1170 的 SKSE 2.2.6）
4. ENB 二进制 `d3d11.dll` + `d3dcompiler_46e.dll` 放进游戏目录（合集只含 ENB 预设，二进制要手动）
5. 混合显卡：强制 Proton 走 NVIDIA（PRIME offload），否则可能跑在 AMD 核显上
6. `apply`（`run` 会自动做）、插件顺序（合集会应用）

---

## 七、首次完整安装结果（69b35e7）— 2026-10-07 01:19

**总结果：`incomplete`，退出码 4**
`installed 2388` / `failed 22` / `pending 34`（全部 `fomod_choices`）/ `skipped 0`（合计 2444）。

**磁盘**：downloads 140G + mods 171G = **312G / 477G**（剩 165G）。解压扩展比 ≈ **1.22x**（140G→171G），与预估 1.1–1.3x 吻合。
**下载阶段**：全量 149.83GB 下完，`rate`/`eta` 可用；总耗时约 2.5–3h（代理 ~12–18MB/s）。

### 7.1 的 22 个 `failed`（下次 `collection install` 会自动重试）

| 原因 | 数量 | 典型 | 性质 |
|---|---|---|---|
| `network_error: request failed: SSL connect error` | 15 | FYX 系列、**Lux Orbis (main)**、Wyrmstooth、Windhelm Objects SMIMed… | 代理网络抖动，重跑多半能好 |
| `io_error: cannot move extracted files into place: 文件名过长` | 6 | `Draugrs - My patches … (一长串) … Xavbio …` 系列 | **mo-linux 缺陷 A12** |
| `invalid_argument: invalid XML: encoding specified in XML declaration is incorrect at line 1` | 1 | Hidden Hideouts of Skyrim | **mo-linux 缺陷 A13** |

- **A12（缺陷，阻断）**：mod 目录名直接取 Nexus 长名，`Draugrs` 补丁那种超长名 + 内部路径 → `ENAMETOOLONG`。建议目录名截断到安全长度 + 短哈希（并保证 modlist/meta 一致）。
- **A13（缺陷）**：FOMOD 的 `moduleconfig.xml` 若 encoding 声明与实际不符（MO2 能容忍），mo-linux 的 XML 解析直接报错。建议宽松解析（忽略/覆盖错误 encoding 声明，按 UTF-8/UTF-16 BOM 检测）。

### 7.2 的 34 个 `pending [fomod_choices]`

两种原因：
- **A14（缺陷/兼容）**「the collection's FOMOD choices **do not fit this archive**」：如
  - `Skyrim Unbound Reborn`：`group 'Timing is Everything' needs exactly one choice`
  - `(ESL) Immersive World Encounters AddOn`：`no plugin 'Finish Installation' in group ' ' of step 'Installation'`
  → 清单（Vortex）记录的 options 与归档 FOMOD 的结构/命名对不上。怀疑：`choices_from_vortex` 与真实 FOMOD 的匹配存在模式差异（只读介绍页/`Finish Installation` 页、单选组、大小写、HTML 转义）。
- **A15（缺陷）**「this FOMOD installer **needs choices**」：清单里根本没给该 mod 的选择。
- **影响**：pending 里含**核心视觉 mod**——SMIM、Skyland AIO、Lux (main)、Embers XD 2K、Majestic Mountains、Beyond Skyrim - Bruma/Assets、Particle Patch for ENB 等，**不能算装全**。
- **注意**：`collection inspect` 的预检把 386 个 FOMOD 都当成「有选择」，**但它并不能预测「选择能不能对上归档」** → 预检给了过于乐观的信号（可考虑在 inspect 里对 FOMOD 做一次「完整性」抽查）。

---

## 八、硬件事故：USB SSD 掉线（2026-10-07 01:56）

**第三轮 `--fomod-defaults`（补装 27 个无选择 FOMOD）跑到 `install 2235/2444`（正在装 Lux (main)）时，`/dev/sda` 从 USB 总线消失。**

dmesg 证据：
- `sd 0:0:0:0: [sda] tag#N FAILED Result: hostbyte=DID_ERROR driverbyte=DRIVER_OK cmd_age=47s`（命令 47 秒无响应，多块并发超时）
- `I/O error, dev sda, sector … op 0x1:(WRITE) …`（读写同时失败）
- `BTRFS error … error while writing out transaction: -5` → `Transaction 446 aborted` → **`forced readonly`**
- 随后 `/sys/class/block/sda/device/state = offline`、`/dev/sda1` 打不开、`lsusb` 里已无该设备

**影响**：第三轮 install 进程死亡（exit 1）；`/mnt/skyrim` 变 `ro` 且读不了；`downloads 140G + mods 171G` 的可用性待重新挂载后确认。**未安装的剩余 ~9 个 mod（含 Lux main、Skyrim AIO 之后的）以及 27 个 FOMOD 默认项未完成。**

**性质**：持续高强度随机写（解包 2444 个 mod）下 U 盘掉线——**典型廉价 USB SSD / UAS 链路不稳或过热/供电问题**，非软件缺陷。这也解释了作者为什么强调「SSD strongly recommended」。

**处置建议**：重新插拔（换 USB 口/线，直连不用 hub，优先 USB3）；回来后在**只读**下 `btrfs check`/`scrub` 评估损伤；若反复掉线，改用内置 NVMe（`/home` 尚余 519G）或换盘。

---

## 九、合集装完（2026-10-07 ~02:00，盘恢复后）+ 启动前阻挡项

**结果：`complete` — 2444 installed / 0 failed / 0 pending，退出码 0。**（清理掉线残留的 `.mol-extract/.mol-stage-<pid>` 后，`collection install 9zfscf --fomod-defaults` 收尾。）
盘恢复干净：`device stats` 全 0、回滚到 `transid 445`、重挂后无新错误。用户选择**不迁回系统盘**。

### 9.1 `next`（新版本）已正确领导
```
- game.downgrade | blocking | needs_human   → collection '9zfscf' targets 1.6.1170.0 but game is 1.7.104.0
- farm.warnings  |            needs_human   → 2 merge warning(s) (case conflicts)
- farm           → apply
- plugins.masters …（28 条 error）
```
**A11 修复确认**：`next` 现在第一步就是 `game.downgrade`（`needs_human`），不再误导去装 1.7.104 的 SKSE。

### 9.2 启动前阻挡项（`doctor` 28 error / 17 warn）
1. **游戏降级 `1.7.104.0 → 1.6.1170`**（needs_human，mo-linux 不管）
2. **缺 AE/CC 内容（7 个？）**：游戏 `Data` 里 `cc*` = **140**（应 148）。`doctor` 具体点名缺这 4 个 master：
   `ccbgssse010-petdwarvenarmoredmudcrab.esl`、`ccbgssse064-ba_elven.esl`、`ccbgssse011-hrsarmrelvn.esl`、`ccbgssse012-hrsarmrstl.esl`
3. **3 个非 CC master 缺失**（mods 里找不到对应 `.esp`，疑似 FOMOD 条件补丁/默认选择未产出）：
   - `AI Overhaul.esp`（`JKJ - AIO Patch.esp` 需要）
   - `Requiem - Immersive College of Winterhold.esp`（`Constellations - Balancing/Compatibility.esp` 需要）
   - `Skyrim Unbound Addon - Bruma.esp`（`Constellations - Unbound.esp` 需要）
   → **待查**：可能是 FOMOD `fileDependency`（依赖已安装 mod 才出现补丁）在合集安装期的求值问题，或 `--fomod-defaults` 的默认未含该补丁。**记为 A16（待确认）**。
4. **SKSE 不匹配**：`skse64_1_7_104.dll not found` → **必须在降级之后** `skse install`（才装 1.6.1170 的 SKSE 2.2.6）
5. 插件顺序：`plugins sort`（一批 "loaded before its master" 警告，可自动修）
6. 载入前还需：`apply`、ENB 二进制、强制 NVIDIA（混合显卡）

### 9.3 其他观察
- `collection install --fomod-defaults` 是**收尾的关键**：27 个「清单未记录选择」的 FOMOD 全部按安装器默认装上了（`--fomod-defaults` 只对无选择项生效，不影响有清单选择的 359 个）。**副作用**：默认选择可能与策展人意图不同 → 见 9.2 第 3 点。
- 掉线事故对数据**无可见损伤**（btrfs 回滚到一致点）——btrfs 的表现值得肯定。

### 9.4 Linux 降级到 1.6.1170 的方法（已备工具）
- **首选**：`JackifyGameDowngrader`（GPLv3，Linux/Windows，用 Valve 官方 SteamCMD 下 depot、自动备份、设 Steam「仅启动时更新」、删 `ContentCatalog.txt`）。
  已下载解压到 `~/Apps/jackify-downgrader`（v0.2.6，`list-versions --game skyrim_se` = 1.6.1170/1.6.640/1.5.97）。
  运行（**需交互输入 Steam 账号/密码/Steam Guard，故须人跑**）：
  ```sh
  cd ~/Apps/jackify-downgrader && ./jackify-game-downgrader --game skyrim_se
  # 选 1.6.1170；建议做全量备份(~17GB)；之后 Steam 会被关闭再重启
  ```
- **备选（纯 Steam，无需第三方）**：Steam 控制台（`steam://open/console`）依次执行，然后合并 depot 到游戏根目录：
  ```
  download_depot 489830 489831 8442952117333549665
  download_depot 489830 489832 8042843504692938467
  download_depot 489830 489833 1914580699073641964
  ```
  （来自 Nexus 文章 12471）
- **降级后**：**不要通过 Steam 启动原版游戏**（否则可能被更回 1.7.104），一律经 mo-linux/SKSE 启动。之后再查 `cc*` 数量是否回到 148。

### 9.5 AE/CC 补齐（2026-10-07 ~02:30）
- depot（489831/832/833）= **完整基础游戏 1.6.1170**（含 4 个免费 cc 包 + `Skyrim.ccc` + 5 个 ESM + 全部 BSA + exe），**不含 AE 的 cc**。
- 做法：**经 Steam 启动原版（1.7.104）→ 游戏内 Creations 下载 → 原版 `Data` 的 cc 从 140 涨到 148**（补上 `ccbgssse010/011/012/064` 等）。
- 副本固化：`find 原版/Data -iname 'cc*' → cp -a` 到 `副本/Data`（先清副本旧 cc* 避免大小写重名）→ 副本 cc = 148。**`Skyrim.ccc` 原版/副本都是 74 行**（它并非全部 cc 的清单）。
- 观察：**直接 `proton run 副本/SkyrimSE.exe`** 会在约 8 秒后退出（Steam 认到了该 app 进程，DXVK 设备建在 RTX 5060 上、SteamAPI 正常），日志末尾是 `seh:call_handler`（异常被处理）。疑似「1.7.104 的 Data 残余 + 1.6.1170 depot」混装。待验证：让 Steam 预编译 shader 后重试 / 纯 depot 重建。

---

## 十、原版启动崩溃 + 缺前置修复（2026-10-07 ~03:10）

### 10.1 原版（clean）1.6.1170 启动 ~8 秒闪退
- **根因**：用 1.7.104 下载 CC 时改写了 `pfx/.../AppData/Local/Skyrim Special Edition/ContentCatalog.txt`，其中有一条 `"Version" : "1701307962.== Version Number =="`（Elven Hunter）。1.6.1170 解析时 `MSVCP140` 抛 C++ 异常（`e06d7363`，PROTON_LOG 里可见）→ 进程退出。
- **处置**：挪开为 `ContentCatalog.txt.1.7.104` → 原版正常进主菜单。**以后每次用 Steam 跑 1.7.x 都会重写它，降级后要再删一次。**（Jackify 降级器也会删它。）

### 10.2 A16 确认并修复：缺 master 的真正原因
1. **A17 清单 `hashes`（Vortex「复刻」安装）没被实现**：88 个 mod 带 `hashes`（策展人装出的每个文件 path+md5），Vortex 用 list installer **只**装这些文件。mo-linux 当成「无选择 FOMOD」用默认装 → Requiem Patch Central 缺 ICOW esp/bsa、JKJ 多出 AIO 补丁。→ 新增 `FomodMode::Replicate`，旧安装按 `mol_fomod=replicate:<指纹>` 自动原地重装。
2. **A18 清单选中的 `NotUsable` 插件被丢弃**：Skyrim Unbound 的 Bruma 选项依赖 `BSHeartland.esm`，按安装顺序当时还没装 → 被判 NotUsable 忽略。→ 宽松模式下有文件的照装；`fileDependency` 把清单插件列表视作 Active。
3. **A19 `Data/` + readme 的压缩包没剥 `Data`**：Interesting NPCs 4.53 Hotfix 等 5 个 mod 的内容一直在 `mods/X/Data/…`（不生效）。复刻后主 mod 不再带旧 `3DNPC.esp`，暴露出 87 个 FAIL。→ 顶层有 Data 且其余非游戏数据时以 Data 为根。
4. **A20 整包放在 `Fomod/` 下的被当包装剥掉 → FOMOD 没识别、原样全装**：Unofficial Lux Patchhub、Modern Wait Menu NORDIC UI Patch。→ 永不剥 `fomod` 目录。
- 新增 `collection resolve --mod KEY --reinstall`。结果：`doctor` 0 error；`apply`（217548 ops）、`plugins sync` 完成；`next` = ready。
- 剩余 2 条 merge 警告（1SpriggansSE `meshes/Meshes`、Skyland Update `textures/Textures`）是压缩包自带的大小写重名目录，farm 按 casefold 合并，文件不丢，无害。

## 十一、SKSE 插件大面积加载失败（2026-10-07 ~03:15）
- 现象：`run --skse` 后 SKSE 弹窗列出几十个 `disabled, fatal error occurred while loading plugin` / `couldn't load plugin (000003E6)`；插件自身日志一行未写。
- 根因：前缀 `msvcp140.dll` = 14.00.24215（VC++ 2015），新 CommonLibSSE-NG 插件需 ≥ 14.40。
- 修复：`protontricks 489830 -q vcrun2022` → 14.44.35211；再启动 **194 个插件全部 loaded correctly**，CrashLogger 正常，游戏存活无崩溃日志。
- 同时修了 A21（Windows zip 的 `\` 路径：Skyrim Priority 的 `PriorityMod.dll` 原来在 farm 根以 `Data\SKSE\Plugins\PriorityMod.dll` 为文件名）。
- 仍缺：ENB 二进制（KiLoader 弹窗：ENBHelperSE / KiENBExtender 需要 d3d11.dll），点 No 可继续。
- 全部转为自动化计划：`docs/PLAN-autofix.md`。

## 十二、进了主菜单，但一个 mod 插件都没加载（2026-10-07 ~03:30）
- 现象：主菜单弹 `Dismembering Framework.esm is missing`；profile 的 `plugins.txt` 在 03:21 被游戏重写，2375 行**没有一个 `*`**。
- A23 根因：02:46 原版启动留下真实 `AppData/…/Plugins.txt`（空列表），03:08 `plugins sync` 建了小写 `plugins.txt` 链接，两者并存；Wine 优先大小写一致的 `Plugins.txt` → 游戏读空列表。修复：sync 前备份大小写影子。
- A22（顺带发现）：清单插件列表 35 个重复名 → `apply_plugin_spec` 把末尾的 DynDOLOD/Synthesis/Occlusion/Constellations 补丁挤出 plugins.txt。修复：去重。
- 恢复：`collection install`（重新套用清单）→ 2449 个启用、0 重复、DynDOLOD 等在末尾；`plugins sync` 后前缀只剩我们的链接。
- 注意：首次加载约 5 分钟（2400+ 插件、USB 盘），CPU ~97% 单核，是正常的。
