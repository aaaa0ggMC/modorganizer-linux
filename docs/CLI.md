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
退出码：0 成功；1 运行期错误；2 用法错误（未知命令/缺参数，此时也输出 envelope，code=`invalid_argument`）；3 `status` 检测到漂移。
日志一律走 stderr，stdout 只有结果。

## 进度事件（`--events`）
每行一个 JSON：`{"event":"progress","op":"apply","done":120,"total":5000}`；开始 `{"event":"start","op":"apply"}`；结束 `{"event":"done","op":"apply","ok":true}`（运行失败时为 `ok:false`）。
`progress` 至多每 50ms 或每 1% 发一次；对端关闭管道（EPIPE）时静默停止发送，命令照常完成。`fifo:`：不存在则 mkfifo，以非阻塞写打开，无读端则放弃（不阻塞命令）。`unix:`：connect 失败则放弃。`fd:`：直接 write。

## 命令
- `instance init [--game-dir G] [--prefix P] [--prefix-user U] [--runner proton|wine] [--proton-path X] [--steam-root S] [--profile N]`
  data: `{"root":"…","changed":bool,"config":{…mo-linux.json 内容…}}`
- `instance show`  data: `{"root","mods_dir","profiles_dir","downloads_dir","overwrite_dir","farm_path","config":{game,game_dir,prefix,prefix_user,profile,farm_dir,runner_kind,proton_path,steam_root}}`
- `mods list`  data: `{"profile":"…","mods":[{"name","enabled","separator","exists","priority","path"}]}`（低→高优先级）
- `mods enable NAME` / `mods disable NAME`  data: `{"name","enabled":bool,"changed":bool}`
- `mods move NAME --to N`  data: `{"name","priority":N,"changed":bool}`
- `conflicts [--mod NAME]`  data: `{"conflicts":[{"path","winner":"层名","losers":["层名"…]}],"count":N}`；`--mod` 只保留涉及该 mod 的条目。层名 = mod 名 / `<game>` / `<overwrite>`。
- `plan`  只读。data: `{"ops":[{"kind":"mkdir|link|relink|remove|rmdir","path":"…","target":"…"}],"count":N,"counts":{"mkdir":n,"link":n,"relink":n,"remove":n,"rmdir":n},"warnings":N}`
- `status`  只读。data: `{"in_sync":bool,"pending":N,"farm_path":"…","farm_exists":bool}`；`in_sync:false` 时退出码 3。
- `apply`  构建期望树并物化农场（空树首次创建 marker 时也返回 `changed:true`）；data: `{"applied":N,"changed":bool,"farm_path":"…"}`。发 `--events`。
- `unlink`  删除农场（`remove_farm`）；data: `{"removed":bool,"farm_path":"…"}`；农场不存在 → `removed:false`（幂等，不是错误）。
- `game info`  只读；从实例配置加载 `GameHost`（`MOL_GAME_LIB` 可指定库）。data 为游戏层原始信息对象：`{name,shortName,steamAppId,binaryName,launcherName,nexusGameId,gameDirectory,dataDirectory,documentsDirectory,savesDirectory,installed,looksValid,version,primaryPlugins,dlcPlugins,ccPlugins,iniFiles,variants,executables,scriptExtender}`。`executables` 是 `{title,binary,arguments,workingDirectory}` 数组，`scriptExtender` 是 `{name,loader,loaderPath,installed,version,savegameExtension}` 对象（游戏层可用时出现）。库缺失/加载失败/信息无效 → `game_unavailable`，退出 1；不初始化 profile、不启动游戏。
- `version`  data: `{"name":"mo-linux","version":"0.0.1"}`
- `plugins sync`  写 profile 与前缀 AppData（不启动游戏）。profile 无 plugins.txt 时先调用上游 `initializeProfile`；随后把上游 `mappings()` 物化为符号链接（目标处已有真实文件 → 改名 `.mol-backup`，已有备份则拒绝）。data：`{"profile","initialized_profile","changed","entries":[{"source","destination","action"}]}`，action ∈ `ok|link|relink|backup+link|skip-missing-source`。幂等。

- `run [--exe REL] [--skse] [--detach] [--dry-run]`  **会启动游戏**。流程：capture 上次残留的 overwrite → `plugins sync` → apply 农场 → `onAboutToRun` → `proton run`（默认阻塞到游戏退出；`--detach` 立即返回）→ 退出后把农场里的新真实文件移回 `overwrite/`（`Data/` 之外的进 `<实例>/overwrite-root/`）。`--dry-run` 只给出命令、不改任何东西。data：`{exe,dry_run,synced_plugins,game_exit_code,captured,argv,cwd}`。

未给出 `--game-dir/--prefix/--proton-path/--steam-root` 时从本机 Steam 自动探测（libraryfolders.vdf、compatdata/489830/pfx、最新的 Proton）；探测不到才报错。实例目录可用环境变量 `MOL_INSTANCE` 设为默认。

- `overwrite capture`  把农场里游戏新建的真实文件移回 `overwrite/`（`Data/` 下按相对路径；其余进 `<实例>/overwrite-root/`）。目标已有同名文件时旧文件先备份到 `<实例>/overwrite-backup/`。data：`{captured,changed}`。
- `apply`/`unlink`/`run`/`overwrite capture` 在有进程使用农场（命令行含农场路径或 cwd 在农场内，含 Wine 的反斜杠路径）时拒绝，错误码 `farm_busy`。

后续：`doctor`。

## 约定
- 实例与游戏目录输出为绝对 Unix 路径，UTF-8；`plan.ops[].path` 与冲突 `path` 为相对农场根的路径，`plan.ops[].target` 为绝对源路径（无目标时为空）。
- 变更命令在 `--json` 与文本模式下的行为一致，仅输出格式不同。
- 文本模式：一行摘要 + 必要的列表，面向人；不保证稳定，GUI 不得解析。
