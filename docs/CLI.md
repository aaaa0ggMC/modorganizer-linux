# mo-linux CLI 规格

无状态：状态只存在于磁盘（实例目录、农场目录）。变更类命令幂等（重复执行结果相同，第二次 `changed:false`）。

## 全局选项
| 选项 | 说明 |
|---|---|
| `-i, --instance DIR` | 实例目录；缺省取环境变量 `MOL_INSTANCE`，再缺省取当前目录 |
| `-p, --profile NAME` | 覆盖实例配置里的 profile |
| `-j, --json` | stdout 输出 JSON envelope（见下）；否则输出人类可读文本 |
| `--events TARGET` | 进度事件（NDJSON）输出目标：`fd:N` \| `fifo:PATH` \| `unix:PATH` |
| `-q, --quiet` | 不向 stderr 输出日志（默认只输出 warn 及以上） |

## JSON envelope（stdout，仅 `--json` 时）
```json
{"schema_version":1,"ok":true,"command":"apply","data":{...},"warnings":[{"code":"..","message":"..","path":".."}],"errors":[]}
```
失败：`ok:false`，`data` 为 `null`，`errors:[{"code":"farm_not_owned","message":"…","path":"…"}]`（code 见 core/include/mol/error.hpp）。
退出码：0 成功；1 运行期错误；2 用法错误（未知命令/缺参数，此时也输出 envelope，code=`invalid_argument`）；3 `status` 检测到漂移（`doctor` 有 error 时同样为 3）；4 **未完成，需要人介入**（`collection install`/`collection status` 有 pending 或 failed，ok 仍为 true；见下文「交互设计」）。
日志一律走 stderr，stdout 只有结果。

## 进度事件（`--events`）
每行一个 JSON：`{"event":"progress","op":"apply","done":120,"total":5000}`；开始 `{"event":"start","op":"apply"}`；结束 `{"event":"done","op":"apply","ok":true}`（运行失败时为 `ok:false`）。
`progress` 可带 `item`（当前处理的条目，如集合里正在下载/安装的 mod 名：`{"event":"progress","op":"install","done":3,"total":68,"item":"SkyUI"}`），item 变化的那一次不节流；`progress` 至多每 50ms 或每 1% 发一次；对端关闭管道（EPIPE）时静默停止发送，命令照常完成。`fifo:`：不存在则 mkfifo，以非阻塞写打开，无读端则放弃（不阻塞命令）。`unix:`：connect 失败则放弃。`fd:`：直接 write。

## 命令
- `instance init [--game-dir G] [--prefix P] [--prefix-user U] [--runner proton|wine] [--proton-path X] [--steam-root S] [--profile N]`
  data: `{"root":"…","changed":bool,"config":{…mo-linux.json 内容…}}`
- `instance show`  data: `{"root","mods_dir","profiles_dir","downloads_dir","overwrite_dir","farm_path","config":{game,game_dir,prefix,prefix_user,profile,farm_dir,runner_kind,proton_path,steam_root}}`
- `mods list`  data: `{"profile":"…","mods":[{"name","enabled","separator","exists","priority","path"}]}`（低→高优先级）
- `mods enable NAME` / `mods disable NAME`  data: `{"name","enabled":bool,"changed":bool}`
- `mods move NAME --to N`  data: `{"name","priority":N,"changed":bool}`
- `conflicts [--mod NAME]`  data: `{"conflicts":[{"path","winner":"层名","losers":["层名"…]}],"count":N}`；`--mod` 只保留涉及该 mod 的条目。层名 = mod 名 / `<game>` / `<overwrite>` / `<overwrite-root>`（农场根的最高层）。
- `plan`  只读。data: `{"ops":[{"kind":"mkdir|link|relink|remove|rmdir","path":"…","target":"…"}],"count":N,"counts":{"mkdir":n,"link":n,"relink":n,"remove":n,"rmdir":n},"warnings":N}`
- `status`  只读。data: `{"in_sync":bool,"pending":N,"farm_path":"…","farm_exists":bool}`；`in_sync:false` 时退出码 3。
- `apply`  构建期望树并物化农场（空树首次创建 marker 时也返回 `changed:true`）；data: `{"applied":N,"changed":bool,"farm_path":"…"}`。发 `--events`。
- `unlink`  删除农场（`remove_farm`）；data: `{"removed":bool,"farm_path":"…"}`；农场不存在 → `removed:false`（幂等，不是错误）。
- `game info`  只读；从实例配置加载 `GameHost`（`MOL_GAME_LIB` 可指定库）。data 为游戏层原始信息对象：`{name,shortName,steamAppId,binaryName,launcherName,nexusGameId,gameDirectory,dataDirectory,documentsDirectory,savesDirectory,installed,looksValid,version,primaryPlugins,dlcPlugins,ccPlugins,iniFiles,variants,executables,scriptExtender}`。`executables` 是 `{title,binary,arguments,workingDirectory}` 数组，`scriptExtender` 是 `{name,loader,loaderPath,installed,version,savegameExtension}` 对象（游戏层可用时出现）。库缺失/加载失败/信息无效 → `game_unavailable`，退出 1；不初始化 profile、不启动游戏。
- `version`  data: `{"name":"mo-linux","version":"0.0.1"}`
- `docs [TOPIC]`  只读。构建时用 `#embed` 编进二进制的文档：不给 TOPIC 列出 `{topics:[{name,file,title,bytes}]}`；`docs guide|agent|cli|handbook|readme`（也接受 `GUIDE.md` 这类文件名）给出全文 `{topic,markdown}`，文本模式直接打印 Markdown。未知 TOPIC → `invalid_argument`（退出 2）。
- `plugins sync`  写 profile 与前缀 AppData（不启动游戏）。profile 无 plugins.txt 时先调用上游 `initializeProfile`；随后把上游 `mappings()` 物化为符号链接（目标处已有真实文件 → 改名 `.mol-backup`，已有备份则拒绝）。data：`{"profile","initialized_profile","changed","entries":[{"source","destination","action"}]}`，action ∈ `ok|link|relink|backup+link|skip-missing-source|skip-existing-directory`。幂等。**还会按 MO2 的 profile 设置做本地 ini/存档映射**：profile 的 `settings.ini` 里 `LocalSettings=true` → profile 目录里的 `Skyrim.ini`/`SkyrimPrefs.ini`/`SkyrimCustom.ini`（大小写不敏感）链接进前缀的 `Documents/My Games/Skyrim Special Edition/`（目标已有真实 ini 先改名 `.mol-backup`；游戏写它们就写回 profile，等价于 MO2 的 usvfs）；`LocalSaves=true` 且 profile 有 `saves/` → 把前缀的 `Saves` 链接过去，**目标是有内容的真实目录时绝不碰**（`skip-existing-directory`）。`run` 会自动执行。

- `executables list`  只读。列出实例里登记的可执行文件（`ModOrganizer.ini` 的 `[customExecutables]`：整合包常登记 SKSE、xEdit、Synthesis、BodySlide 等）。data：`{executables:[{title,binary(Unix 路径),arguments,working_dir,farm_path,hide}]}`；`farm_path` 非空 = 该文件在游戏目录或某个 mod 里，会在农场里按这个相对路径运行（普通 mod 的文件在 `Data/` 下，根目录型 mod 在农场根）；为空 = 在农场之外，直接用 `binary` 的绝对路径运行。
- `run [--exe REL] [--args "A B"] [--skse] [--title NAME] [--no-cow] [--detach] [--dry-run]`  **会启动进程**。流程：capture 上次残留的 overwrite → `plugins sync`（仅游戏本体/SKSE）→ apply 农场 → `onAboutToRun` → `proton run`（默认阻塞到进程退出；`--detach` 立即返回）→ 退出后把农场里的新真实文件移回 `overwrite/`（`Data/` 之外的进 `<实例>/overwrite-root/`）。`--exe` 可以是农场里任意 exe（相对农场根，如 `Tools/Patcher.exe`、`Data/xxx.exe`）；不是游戏本体时不需要游戏层 host 库、不同步插件。`--args` 给 exe 追加参数（按 shell 规则切分，与 `--title` 互斥）。`--title NAME` 运行实例里登记的工具（带它登记的参数，见 `executables list`）。`--dry-run` 只给出命令、不改任何东西。
  **写时复制（COW，默认开启）**：进程里预加载 `libmol-cow.so`（与 `mo-linux` 同目录，或 `$MOL_COW_LIB`）。程序以写方式打开农场里的文件时，先把原文件复制（btrfs/xfs 上为 reflink，瞬间完成且不占空间；ext4 上真复制）成农场里的真实文件再写；删除/改名/移动只作用于农场里的链接；往游戏目录、`overwrite/`、`overwrite-root/` 里直接写/建文件被拒绝（EACCES）。退出后 capture 把改过的副本收进 overwrite（与原文件内容相同的副本直接丢弃），游戏目录与 mod 原文件**一个字节都不变**。`overwrite-root/` 是农场根的最高优先级层，所以被工具改过的 `SkyrimSE.exe` 等下次 apply 后生效；删除 `overwrite-root/` 里的文件即撤销。`--no-cow` 关掉（回到旧行为：改写会穿过链接落到原文件）。同一 Wine 前缀里已有不带 COW 的 wineserver（例如你刚用 `protontricks` 跑过东西）时等它最多 10 秒，仍在则报 `wine_busy`。
  data：`{exe,title,dry_run,synced_plugins,game_exit_code,captured,argv,cwd,cow,cow_library,cow_copies,cow_reflinked}`；`cow_copies` = 本次被复制出来的文件数，`cow_reflinked` = 其中走 reflink 的（btrfs 上应与 cow_copies 相等）。

未给出 `--game-dir/--prefix/--proton-path/--steam-root` 时从本机 Steam 自动探测（libraryfolders.vdf、compatdata/489830/pfx、最新的 Proton）；探测不到才报错。实例目录可用环境变量 `MOL_INSTANCE` 设为默认。

- `overwrite capture`  把农场里游戏新建的真实文件移回 `overwrite/`（`Data/` 下按相对路径；其余进 `<实例>/overwrite-root/`）。目标已有同名文件时旧文件先备份到 `<实例>/overwrite-backup/`。data：`{captured,changed}`。
- `overwrite promote --filter GLOB[,GLOB…] [--yes]`  **破坏性**：把 `overwrite/` 里匹配的文件移进**真实游戏的 `Data/`**（让 Steam 直接启动也能看到，例如 Creations）。不带 `--yes` 只预览。`--filter` 必填（大小写不敏感，匹配相对 overwrite 的路径）；目标已存在则跳过，不覆盖游戏文件；目录名大小写沿用游戏目录里的写法；执行需要农场空闲，之后需 `apply`（会有 `farm_stale` 警告）。data：`{executed,moved,skipped,files:[{path,dest,skipped}]}`。
- `apply`/`unlink`/`run`/`overwrite capture` 在有进程使用农场（命令行含农场路径或 cwd 在农场内，含 Wine 的反斜杠路径）时拒绝，错误码 `farm_busy`。

- `plugins list`  只读。当前 profile 的插件加载顺序：`{profile,changed:false,plugins:[{name,index,enabled,forced,master,light,source,masters:[…]}],issues:[{plugin,master,kind}]}`。可用插件 = 农场里 `Data/` 顶层的 `.esp/.esm/.esl`；`forced` = 游戏自带（Skyrim.esm、官方 DLC 等，永远启用、排最前）；`master` = ESM 标志或 `.esm`；`source` = 提供它的层；`masters` 来自文件头。`issues.kind` ∈ `missing`（master 不在磁盘上）｜`disabled`（master 被禁用）｜`after`（master 排在后面）。排序规则：强制 → ESM 标志 → 其它；列表里有而磁盘上没有的被丢弃，磁盘上有而列表里没有的追加并默认启用。
- `plugins enable|disable NAME`、`plugins move NAME --to N`、`plugins sort`  修改并写回 profile 的 `plugins.txt`/`loadorder.txt`。data 同 `plugins list`，`changed` 表示是否真的改了顺序/状态。禁用强制插件 → `invalid_argument`；不存在 → `mod_not_found`。`move` 不能越过「强制/ESM」区的边界（会被规范化修正）。`sort` 默认只保证每个插件排在其 masters 之后；`plugins sort --loot [--masterlist FILE] [--refresh]` 额外套用 LOOT 社区 masterlist（loot/skyrimse，下载并缓存 24 小时到 `~/.cache/mo-linux/loot`）里的 `after`/`req` 规则与**分组顺序**（Main Plugins → Creation Club → Unofficial Patches → … → default → Low Priority Overrides）。这是对 LOOT 的**近似**：不处理带 condition 的规则、没有 userlist、没有 overlap 启发式；规则无法满足（成环、与 ESM/ESP 分区冲突）时忽略并给 warnings `loot_note`。data 里 `sorted_with`（`masters`|`loot`）、`rules_applied`、`grouped`。
- `run` 启动前会先把插件列表规范化并写回 profile（新装 mod 的插件才会被游戏加载）。
- `doctor`  只读体检。data：`{errors,warnings,checks:[{id,level,message,hint}]}`，level ∈ `ok|warn|error`。检查项：`game.dir/exe/data/version`、`skse.loader/version`（按游戏版本推出 `skse64_<a>_<b>_<c>.dll` 并检查存在）、`prefix`、`runner.proton/steam_root`、`profile`、`mods`/`mods.missing`、`farm`/`farm.warnings`/`farm.busy`、`plugins.masters`（缺失/被禁用的 master 为 error，顺序错误为 warn）、`plugins.link`。游戏版本来自游戏层；host 库缺失只降级为 warn。有 error 时退出码 3（ok 仍为 true）。

- `nexus login [--key-file F]`  从文件或 stdin 读取 Nexus **个人 API key**，**先向 Nexus 验证、通过才保存**到 `~/.config/mo-linux/nexus.key`（0600）。环境变量 `NEXUS_API_KEY` 优先于文件。key 永不出现在输出/日志/错误信息里。data：`{name,user_id,is_premium,is_supporter,key_path}`。
- `nexus logout`  删除保存的 key。data：`{removed}`。
- `nexus whoami`  data 同 login（`key_path` 为空）。
- `nexus files --mod ID`  data：`{game,mod_id,files:[{file_id,name,file_name,version,category,size_kb,is_primary}]}`。
- `nexus download (--nxm URL | --mod ID --file ID)`  下载到实例 `downloads/`，并写 MO2 兼容的 `.meta`；支持断点续传；进度走 `--events`（`download`）。Premium 账号可直接 `--mod/--file`；免费账号必须用网页上点“慢速下载”得到的 `nxm://…?key=…&expires=…` 链接，否则 `nexus_premium`。data：`{path,size,game,mod_id,file_id}`。
- Nexus 错误码：`nexus_auth`(401) `nexus_premium`(403) `nexus_not_found`(404) `nexus_rate_limited`(429，含 Retry-After) `network_error`。基址可用 `MOL_NEXUS_API` 覆盖（测试用）。遵循 http(s)_proxy 环境变量。

- `mods install ARCHIVE [--name N] [--root]`  把压缩包装成 `mods/<name>/` 并以最高优先级启用写入 modlist。解压用外部 `7z`/`7zz`/`bsdtar`（拒绝含符号链接/越界路径的包）；自动去掉多余的单层外壳目录；顶层有 `.exe/.dll` → **根目录型 mod**；只有一个顶层目录时，若它（或其内容）已经是游戏数据目录（`SKSE/`、`meshes/`、`scripts/`……，名单与上游 SkyrimSEModDataChecker 一致）则原样保留，否则当作外壳剥掉（`meta.ini` 写 `mol_root=true`，映射到农场根而非 `Data/`，例如 SKSE64）；顶层只有 `Data/` → 取其内容。已有同名 mod 拒绝。data：`{name,path,root,files}`。`mods list` 的每行多了 `root`。mod 根下的 `meta.ini` 不会进农场。FOMOD：压缩包带 `fomod/ModuleConfig.xml` 时必须指明处理方式，否则报 `fomod_choices_required`：`--fomod CHOICES.json`（显式选择；可见的组缺失则报错）、`--fomod-defaults`（全用默认）、`--no-fomod`（忽略安装器，原样装）。三者互斥。data 多了 `fomod`（是否走了 FOMOD）与 `missing`（FOMOD 引用但压缩包里没有的源，同时进 warnings `fomod_missing_source`）。
- `fomod inspect ARCHIVE [--choices FILE] [--images DIR]`  只读（`--images` 只往 DIR 写图片）。data：`{has_fomod,module_name,steps:[{name,visible,groups:[{name,type,explicit_choice,plugins:[{name,description,image,image_path,type,selected}]}]}],files:[{source,destination,folder,priority}]}`。`--images DIR`：把各选项引用的图片（路径相对模块根、大小写不敏感）解到 DIR，`image_path` 是解出的绝对路径（解不出为空）；只解图片，不解整个压缩包，需要 7z/7zz。`--choices` 给部分/全部选择，未给的组用默认；步骤可见性与插件类型按**此前步骤设置的标志**求值，所以 GUI 每改一次选择就带累计的 choices 再调一次。choices 文件格式：`{"steps":{"<步骤名>":{"<组名>":["<插件名>",…]}}}`。
- `skse install`  一键：由游戏版本推出运行时 dll（`skse64_<a>_<b>_<c>.dll`）→ 已就绪则不做任何事 → 否则在 Nexus（mod 30379）选主文件，下载（已下载则复用），装为根目录型 mod `SKSE64` → 校验 dll 与游戏匹配。需要 Nexus API key 与游戏层 host 库。幂等。错误码 `skse_mismatch`：最新的 SKSE64 还不支持该游戏版本。data：`{game_version,runtime_dll,installed,mod_name,file_name,file_id,downloaded}`。

### 面向 Agent 的命令（详见 docs/AGENT.md）

- `schema`  自描述。data：`{tool,version,envelope,exit_codes,errors:[{code,hint}],global_options,commands:[{name,summary,effects[],needs[],confirm,idempotent,positionals[],options[{name,long,short,takes_value,description}]}]}`。`effects` ∈ `read|instance|farm|prefix|game_dir|launch|network`；`needs` ∈ `instance|nexus_key|host`；`confirm:true` = 执行前应向用户确认（`run`、`overwrite promote`）。
- `next`  只读。data：`{ready,instance,steps:[{id,why,command[],effects,blocking,needs_human,confirm}]}`。来源：`doctor` 的各项检查（带 `fix` 的给出可直接执行的 argv，没有 `fix` 的 `needs_human:true`）、未完成的集合安装、缺少 Nexus key。`ready` = 没有 blocking 步骤；此时最后一步是 `run`（`confirm:true`）。没有实例时第一步是 `instance init`。
- `overview`  只读，给 GUI 概览页用：一次取齐、同一进程里 doctor 与游戏层各只跑一次（代替 `version`+`instance show`+`doctor`+`next`+`mods list`+`plugins list` 的多进程往返）。data：`{version,has_instance,instance,profile,game_dir,game_version,farm_path,nexus_key,mods_total,mods_enabled,mods_missing,plugins_total,plugins_enabled,collections:[{slug,name,revision,installed,pending,failed,skipped}],doctor:<同 doctor 的 data>,next:<同 next 的 data>}`。没有实例时 `has_instance:false`，实例字段为空、`doctor.checks` 为空、`next` 只有 `instance init`。`nexus_key` 只表示有 key（不联网验证）。退出码恒为 0（doctor 的 error 体现在 `doctor.errors`）。
- `logs [--file NAME] [--tail N]`  只读。列出前缀里 `My Games/Skyrim Special Edition/SKSE/` 的日志文件（新→旧），或读取其中一个的最后 N 行（默认 80、最多 2000）。data：`{dir,files:[{name,size,modified}],name,tail}`。`NAME` 只能是目录内的纯文件名。
- 所有错误条目（`errors[]`）与警告多了 `hint` 字段：对该错误码的默认下一步建议。`doctor` 的每项检查多了 `fix`（argv 数组，空 = 需要人处理）。

### 默认实例与 nxm:// 链接处理器（免费账号的下载流程）

- `instance default [--set]`  显示/设置**默认实例**（记在 `~/.config/mo-linux/default-instance`）。没有 `-i`、没有 `MOL_INSTANCE`、且当前目录不是实例时回退到它。`--set` 把 `-i`/当前目录那个实例设为默认（不是实例 → `instance_not_found`）。data：`{path,changed}`。
- `nxm register`  把 mo-linux 注册为浏览器的 `nxm://` 处理器：写 `~/.local/share/applications/mo-linux-nxm.desktop`（`Exec=… nxm handle %u`）并尽力 `xdg-mime default`。**会改 MIME 默认关联，执行前向用户确认**。data：`{desktop_file,exec,mime_registered}`。
- `nxm handle URL`  浏览器点「Mod Manager Download」后由系统调用：用链接里的 `key/expires` 下载文件到默认实例的 `downloads/`，并把压缩包记给**正在等它的集合 mod**（扫 `collections/*/state.json` 里按 modId/fileId 对上的）；下次 `collection install` 会复用并照常做 md5 校验。Wabbajack 的手动下载项也只需文件落进 `downloads/`（按大小+hash 匹配）。完成后尽力发桌面通知。data：`{instance,path,size,game,mod_id,file_id,matches:[{collection,key,name}]}`。

### Nexus 搜索与按 id 安装

- 只读的搜索/详情（`nexus search`、`nexus info`、`collection search`）不需要实例：没有实例时按 Skyrim SE 搜，「已安装」一律为 false。
- `nexus search QUERY [--sort relevance|endorsements|downloads|updatedAt] [--count N≤50] [--offset N]`  搜当前游戏的 mod（QUERY 用站内词干匹配；传空串列出榜单）。data：`{game,query,sort,total,mods:[{mod_id,name,author,summary,version,updated_at,endorsements,downloads,installed}]}`；`installed` = 本实例里已有来自该 mod 的安装（靠 meta.ini 的 `modid`）。
- `nexus info --mod ID`  data：`{mod:{…同上},category,requirements:[{mod_id,name,external,url,notes,installed}],dlc_requirements:[…]}`。`requirements` 是作者在站内声明的前置（`external:true` 的是站外工具，`url` 给出下载页）。
- `nexus install --mod ID [--file ID] [--name N] [--requirements] [--fomod F | --fomod-defaults | --no-fomod]`  取主文件（`is_primary` 优先）→ 下载 → 安装 → 写 MO2 兼容的 `meta.ini`（`gameName/modid/fileid`）。目录名默认取站内 mod 名。`--requirements` 递归先装站内前置（深度 ≤4，已装的跳过，自动用 FOMOD 默认值）。**幂等**：已装过（meta.ini 的 modid 匹配）直接 `already_installed`。data：`{status:"complete"|"incomplete",mods:[{mod_id,name,status,mod_dir,note}],pending:[…同 collection 的 pending],external_requirements:[…],dlc_requirements:[…]}`；FOMOD 缺选择/免费账号无法直连 → pending + 退出码 4（与 collection 相同的「incomplete」协议）。
- `collection search QUERY [--sort …] [--count N] [--offset N]`  搜集合。data：`{game,query,sort,total,collections:[{slug,name,summary,endorsements,downloads,revision,mod_count,total_size}]}`。拿到 slug 后用 `collection inspect/install`。
- `mods list` 每行多了 `nexus_id`；`meta.ini` 里的 `version` 也会读（`mods outdated` 用）。
- `mods outdated`  对所有带 `modid` 的 mod，一次（分批）请求取站上当前版本并对比。data：`{checked,outdated_count,mods:[{name,nexus_id,installed_version,latest_version,outdated,updated_at}]}`。`outdated` 只是版本字符串不同，**不是语义化比较**（且站上 mod 的版本字段可能与具体文件版本不同）；已下架的 mod 会被跳过（`latest_version` 为空）。`nexus install` 与 `collection install` 会写 `modid/fileid/version`，旧实例重跑一次 `collection install` 会补写。

### Wabbajack 整合包

`.wabbajack` 是一个 zip：`modlist`（JSON，Archives + Directives）+ 内联数据/补丁。mo-linux 按清单把**实例目录**（即 MO2 便携实例：`mods/`、`profiles/`、`ModOrganizer.ini`……）重建出来；`-i DIR` 就是输出目录，装完它直接是可用的 mo-linux 实例（自动写 `mo-linux.json`）。

- `wabbajack search QUERY [--count N] [--offset N] [--nsfw] [--all-games]`  搜官方画廊（各仓库列表并行获取，缓存在 `~/.cache/mo-linux/gallery`，6 小时）。默认只显示当前游戏、不含 NSFW，**按压缩包总大小升序**（先看到装得起的）。data：`{game,query,total,lists:[{title,machine_url,repository,author,version,description,download_size,archives_size,installed_size,archive_count,nsfw,unavailable}]}`。
- `wabbajack inspect LIST`  LIST = 本地 `.wabbajack`、画廊的 `machine_url`/标题、或 authored-files 下载 URL（画廊来源会下载并按 xxh64 校验后缓存到 `<实例>/.mol-wabbajack-lists/`）。data：`{name,author,version,description,game_type,game_id,nsfw,game_matches,file,archive_count,archive_size,directive_count,supported_directives,sources:[{name,count,size,supported}],directives:[{name,count,supported}],verdict:"full"|"partial"|"none"}`。`verdict` 表示**指令层面**我们能执行多少；还要看 `sources`（`nexus` 需要 key，免费账号会变成 pending；`gamefile` 要求游戏文件与清单一致；`manual/mega/gdrive/…` 必须手动下载）。
- `wabbajack install LIST [--game-dir DIR] [--downloads DIR] [--jobs N]`  下载（Nexus 需要 key，Http/WabbajackCDN 直接下，游戏文件取自游戏目录）→ 校验 xxh64 → 解压 → 执行指令 → 校验每个输出文件。可续跑、幂等：处理完的压缩包记入 `<实例>/.mol-wabbajack/state.json`，已下载的按「大小+xxh64」复用。data：`{status,instance,archives_total,archives_done,files_written,files_failed,pending:[{key(数量),name,kind,detail,url}],failures:[…]}`；未完成退出码 4。`pending.kind`：`manual_download`｜`game_file_missing`｜`unsupported`（带数量的未知指令类型，意味着清单有一部分内容缺失；目前已知的指令都支持）。
- `ArchiveHashPath` 只有压缩包哈希一项时，源文件就是该下载文件本身（例如取自游戏目录的 esm，补丁直接打在它上面），不解压。Nexus 返回 404（作者撤下/替换了该文件）的压缩包记为 `manual_download` pending（detail 说明原因），可从别处找到同一文件放进 downloads/（按大小+hash 匹配）。装完后实例的 profile 取清单自带的 `ModOrganizer.ini` 里的 `selected_profile`。
- 已支持的指令：`FromArchive`（含嵌套压缩包；**Bethesda BSA 也能当压缩包**——游戏目录里的 CC BSA 常被清单当作来源）、`PatchedFromArchive`（OctoDiff）、`InlineFile`、`RemappedInlineFile`、`MergedPatch`（把已落地的若干文件依次拼接作基础，再打 OctoDiff 补丁，校验输出 hash）、`TransformedTexture`（把源 DDS 解码→缩放到清单给的宽高→按清单的 DXGI 格式（BC1/2/3/4/5/7、RGBA8/BGRA8）和 mip 数重新编码；**近似实现**，视觉等价但字节与作者用 DirectXTex 的结果不同，所以不校验 hash，只给 note）、`CreateBSA`（等其它指令把散文件落到 `TEMP_BSA_FILES\\<TempID>\\…` 后，按作者的版本/标志（SSE v104/105、压缩 LZ4 帧、嵌入文件名）重新打包；重建的 BSA 与作者构建的字节可能不同，只给提示 `wabbajack_note`，BSA 里每个文件仍逐个校验过）（路径占位符 `GAME/MO2/DOWNLOAD_PATH_MAGIC_*` 换成 `Z:\…` 形式的本机路径）。

### Collections（Nexus 集合/整合包）与交互设计

mo-linux 不能在中途向用户提问，所以统一用「**可续跑 + 返回 incomplete**」：每个需要人介入的 mod 记为 pending（不阻塞、不猜测），其余 mod 继续处理；命令最后返回退出码 4 与 pending 清单。用户（或 GUI）用 `collection resolve` 记下决定，再跑一次 `collection install` 即从中断处继续（状态存 `<实例>/collections/<slug>/state.json`，每个 mod 处理完立即落盘；已装好的不会重做，已下载的按「大小+md5」复用，无需联网）。

- `collection inspect COLLECTION [--revision N]`  只读（需要 Nexus key）。COLLECTION 可以是 slug、集合页面 URL（`https://www.nexusmods.com/games/<游戏>/collections/<slug>`）或本地 `collection.json`/清单压缩包。data：`{name,slug,author,domain,revision,game_versions,game_version(本机),mod_count,total_size,plugin_count,rule_count,install_instructions,mods:[{key,name,version,optional,source_type,mod_id,file_id,has_fomod_choices,has_patches,status}]}`，mods 按安装顺序；`status` ∈ `new|pending|installed|skipped|failed`。
- `collection install COLLECTION [--revision N] [--no-optional] [--fomod-defaults] [--jobs N] [-p PROFILE]`  下载（Premium 直连；需要 key）→ md5 校验（不符则删文件并 failed）→ 安装（FOMOD 选择取自清单）→ 按安装顺序（phase、`before/after` 规则）放到 modlist 最高优先级 → 应用清单的插件顺序与启用状态。`-p` 指定装进哪个 profile（不存在则创建；默认当前 profile）。data：`{name,slug,revision,profile,status:"complete"|"incomplete",installed,skipped,failed,plugins_applied,mods:[{key,name,status,mod_dir,note}],pending:[{key,name,kind,detail,url,archive,decision}],notes:[…]}`。**未完成时退出码 4**。`failed`（网络错误、md5 不符等）在下次运行时自动重试。清单与实际游戏版本不一致、有未强制执行的 requires/conflicts 规则时进 `notes`（同时是 warnings `collection_note`）。
  - pending 的 `kind`：`manual_download`（免费账号/浏览器/手动来源；`url` 是页面）｜`fomod_choices`（压缩包有 FOMOD 但清单没给选择，或清单的选择与压缩包对不上；`archive` 是已下载的压缩包，可直接 `fomod inspect`）｜`unsupported`（带二进制补丁 patches 的 mod、bundle 来源）｜`skse`（清单里的 SKSE64 是 silverlock.org 外部来源：执行 `skse install` 后再 `collection install`；实例里已有 SKSE（游戏目录或根目录型 mod 提供 `skse64_loader.exe`）则直接算 installed）。`decision`（仅 `collection status`）：已用 `collection resolve` 记下、等下次 install 生效的决定（`skip`/`fomod_defaults`/`fomod_choices`/`archive`），没有为空。`kind`/`url` 记在 state.json 里，`collection status` 也给出（旧状态文件里没有时 `kind` 为 `pending`）。
  - 同一个 Nexus 文件已经装在实例里（另一个集合、`nexus install`、MO2；按 `meta.ini` 的 `modid`/`fileid`）→ 直接复用该目录，`note` 为 `already installed (same Nexus file)`。目录名冲突按清理后的名字、大小写不敏感判断，冲突时加 ` [tag]` 后缀。
  - 清单记录的 FOMOD 选择里若有 `NotUsable` 插件（Vortex 会把 SelectAll 组里只有说明文字的「介绍」页记成已选），按 MO2 语义忽略它，组的数量约束照常检查。
- `collection readme COLLECTION [--revision N]`  集合页面上作者写的说明（Markdown：安装须知、游戏版本/降级要求、可选项……），不需要实例。`COLLECTION` 可以是 slug、`https://www.nexusmods.com/games/<域名>/collections/<slug>` 或 `https://next.nexusmods.com/<域名>/collections/<slug>`。文本模式直接输出 Markdown。data：`{name,slug,revision,url,summary,description,changelog,markdown,cached}`。有实例时缓存为 `<实例>/collections/<slug>/readme-<rev>.md`；离线或没有 key 时读缓存（`cached:true`，只有 `markdown`）。`collection inspect` 的 data 也多了 `url,summary,description,changelog`。
- `collection status COLLECTION`  只读、离线。data 同 install（来自 state.json）；未完成时退出码 4。
- `collection resolve COLLECTION --mod KEY (--skip | --fomod FILE | --fomod-defaults | --archive FILE | --nxm URL)`  记下对某个 pending mod 的决定（KEY 取 pending 里的 `key`，即清单里的 mod tag）。`--nxm` 会立刻用该链接下载文件（链接的 mod/file id 必须与清单一致），随后 `collection install` 按 md5 找到并安装；`--archive` 是用户自己的文件（不做 md5 校验）。

**并行下载**：`collection install` 与 `wabbajack install` 先并行下完所有需要联网的压缩包（`--jobs N`，1–16，默认 4，或环境变量 `MOL_JOBS`），再按顺序串行安装，所以安装顺序与优先级不变。进度事件：`download`（所有并行下载的合计字节）、`downloaded`（第几个下完）、`install`/`archive`（第几个）。

遵循 http(s)_proxy。URL 里的空格/引号会自动做百分号编码（Nexus 的 CDN 地址里带空格）。

后续：FOMOD 的图片提取、Nexus SSO（需向 Nexus 注册应用 slug）、mod 更新检查。

## 约定
- 实例与游戏目录输出为绝对 Unix 路径，UTF-8；`plan.ops[].path` 与冲突 `path` 为相对农场根的路径，`plan.ops[].target` 为绝对源路径（无目标时为空）。
- 变更命令在 `--json` 与文本模式下的行为一致，仅输出格式不同。
- 文本模式：一行摘要 + 必要的列表，面向人；不保证稳定，GUI 不得解析。
