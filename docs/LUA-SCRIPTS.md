# Lua 安装脚本（execute lua scripts）

Lua 安装脚本把「下载 → 解包 → 跑安装器 → 整理成 mod → 部署」写成可审查、可重跑的脚本。
设计动机、安全模型与取舍见 [`DESIGN-lua-scripts.md`](./DESIGN-lua-scripts.md)；本文只描述**已落地**的接口与边界。

```bash
mo-linux script run scripts/example-stage.lua            # 跑（默认走后台 service）
mo-linux script run x.lua --local --dry-run              # 当前进程 + 临时虚拟根，不碰实例
mo-linux script run x.lua --ns skyrim-main --timeout 120 # 自定义 namespace / 墙钟
mo-linux serve --detach                                  # 手动拉起常驻 service
mo-linux serve --stop                                    # 让它退出
```

## 首版承诺

> 一个脚本及其拉起的 exe，**默认只能写「虚拟根目录」**（`<实例>/scripts/<脚本名>.work`）和它跑安装器
> 所必需的 Wine 前缀。读可以读系统（exe 需要读 prefix/游戏数据/系统库），但 Lua 侧本身只能看见虚拟根。
> WP2 起显式开放了实例写 API（`instance.create` / `mods.*` / `farm.apply`）——这些调用与对应的 CLI 命令
> 同权同校验，`--dry-run` 下一律拒绝，同一实例的并发写由宿主串行化。

- 不从 mod 压缩包、集合清单里自动执行 Lua——只有你显式 `script run` 的文件才会跑。
- 没有 `io` / `os`(除 `clock`/`time`/`date`) / `package` / `debug` / `load` / `require` / `pcall`。
  `string` 只留安全子集（无 `match`/`gmatch`/`gsub`/`format`——原生 C 里跑，hook 断不掉）。
- **失败即停**：宿主调用失败 = Lua 错误冒泡到顶层，整个脚本结束。脚本里没有 `pcall` 可以吞掉它。
  脚本侧校验失败走 alib6 的 panic：用户看到的 error 形如 `invalid state key (lua_script.cpp:1415)`，
  自带 file:line，排查不用猜。
- 所有路径都是**虚拟路径**：相对、`/` 分隔、组件不能是 `.`/`..`、不允许 `\`、`:`、绝对路径。
  符号链接一律不跟随（脚本创建不了链接；归档解出的链接会被拒绝；exe 在虚拟根里造的链接也指不出去）。

## API（v1）

```lua
host.name()                      -- 'mo-linux'

log.info(msg)                    -- msg ≤ 4 KiB；进结果 log[] 与 HTTP /<ns>/log

-- 虚拟文件系统（路径相对于虚拟根）
fs.read(path) -> string|nil      -- ≤ limits.file_bytes
fs.write(path, data) -> true
fs.append(path, data) -> true
fs.exists(path) -> bool
fs.size(path) -> number|nil
fs.list(path) -> {names...}|nil  -- 只有直接子项，已排序
fs.mkdir(path) -> true           -- mkdir -p
fs.remove(path) -> true          -- 递归删（只在虚拟根内）
fs.copy(src, dst) -> true        -- 只复制普通文件
fs.move(src, dst) -> true

-- 归档（外部 7z/7zz/bsdtar；解完拒绝符号链接与越界条目）
archive.list(path) -> {entries...}
archive.extract(path, dest) -> files

-- 网络（只允许 http/https；大小/次数/超时都有限额）
net.get(url, {headers=…, timeout_ms=…}) -> {status=…, body=…}
net.download(url, path) -> bytes  -- path 在虚拟根内

-- 进程：只跑 Windows exe
proc.run(exe, {args={…}, env={…}, timeout_ms=…, cwd=…})
   -> {exit=…, log=…, arch=…, landlock=…, timed_out=…}

-- 状态：扁平 key → 标量（HTTP 可看可改）
state.get(key, default) -> value
state.set(key, value) -> true    -- string/number/boolean
state.delete(key) -> true
state.keys() -> {keys...}

-- 实例（WP2：写实例的 API；--dry-run 下一律拒绝）
instance.create{root=…, game_dir=…, prefix=…, prefix_user=…, profile=…,
                runner='proton'|'wine', proton_path=…, steam_root=…, game=…} -> {root,changed,game,profile}
instance.info() -> {root,game,game_dir,prefix,profile,farm,mods,downloads,overwrite,runner}

-- mod（同样受 dry-run 保护；同一实例的并发写由宿主串行化）
mods.install_archive(vpath, name, {fomod='defaults'|'choices'|'raw', choices={…}, root=bool, replace=bool})
    -> {name,path,root,files,fomod}
mods.install_staged(name, vpath, {root=bool, replace=bool}) -> {name,path,root,files}
mods.enable(name) / mods.disable(name) -> {name,enabled,changed}
mods.list() -> {{name,enabled,exists,root,version,nexus_id,priority}, …}

-- 部署
farm.apply() -> {applied,changed,farm}      -- 农场被占用时抛错（与 `run` 同一保护）
```

`instance.create` 让**一个脚本从零装起一个游戏**：建实例 → 下载/解包 → 跑安装器 exe →
`install_staged` 装成 mod → `farm.apply` 部署。路径必须是绝对路径（这些 API 本来就写在虚拟根之外），
`game_dir` 必须已存在；没有实例时用 `script run --no-instance` 起跑。

装 mod 的布局规则与 `mods install` 完全一致：顶层只有 exe/dll → 根目录型；顶层有 `Data/` 且旁边只有
说明文档 → `Data/` 的内容成为 mod 根；只有一个包装目录 → 剥掉。`install_staged` 是**复制**（不是移动），
虚拟根里的原件不动；源目录里有符号链接或越界条目 → 明确拒绝，且不留下半个 mod。

### `proc.run` 的收容

- 只接受 `.exe`（大小写不敏感）、位于虚拟根内、**PE 头可解析**的文件；ELF/脚本/目录一律拒。
  PE 机器类型会检测并回报（`arch`）：`i386` / `amd64`；`arm64` 拒绝（本主机只能跑 x86）。
- cwd = 虚拟根（或你给的子目录），stdout/stderr 落盘到 `<虚拟根>/.mol-logs/proc-N.log`（`log` 返回相对路径，
  可以 `fs.read` 回来）。到 `timeout_ms`（默认 15 分钟）杀整个进程组。
- **Landlock**（内核 ≥ 5.13）对 exe 进程树施加：读全盘、**只写虚拟根 + Wine 前缀 + 临时目录**。
  结果里的 `landlock` 为 `v1`（已收容）或 `unavailable`（老内核：**拒绝运行**，宁可失败也不静默失保护）。
- 32 位 exe 也能被收容：Landlock 按路径授权、与位数无关；`LD_PRELOAD`  hook 对 WoW64 下的 32 位程序
  本来就越过（见 DESIGN §2.5），所以这里**不依赖** hook。检测到 i386 时会记一条 note。
- `env` 只允许 `[A-Za-z_][A-Za-z0-9_]*` 键，且拒绝 `LD_*` / `MOL_COW_*` / `WINEPREFIX` / `WINELOADER*` /
  `WINEDLL*`（摘不掉注入）；宿主的 runner 变量在应用顺序上排在脚本 env 之后。
- exe 的网络**没有**被切断（一批安装器要联网）；需要时见 DESIGN §WP4。

## 限额（`Limits`，都可用 CLI 选项调）

| 项 | 默认 | CLI |
|---|---|---|
| 源码大小 | 256 KiB | — |
| Lua 堆 | 32 MiB（夹到 [1 MiB, 128 MiB]） | `--memory MB` |
| 指令预算 | 4e9 | — |
| 整脚本墙钟 | 60 min | `--timeout MIN` |
| 单文件读写 | 2 GiB | — |
| 虚拟根总写入 | 16 GiB | — |
| `fs.*` 调用次数 | 100k | — |
| `proc.run` 次数 | 32 | — |
| 单个 exe 墙钟 | 15 min | `timeout_ms` |
| `net.*` 次数 / 单次响应体 | 512 / 256 MiB | — |
| state 键数 / 单值 | 4096 / 256 KiB | — |
| 日志行数 | 2000（丢最旧） | — |

死循环、内存炸弹、写爆磁盘都会被截成一条结构化错误（`ok=false` + 可操作 `error`），不挂死不 OOM。

## HTTP namespace：从浏览器看/改脚本状态

`script run` 会起一个只绑 `127.0.0.1` 的 HTTP server，每个 run 一个 namespace（默认 `script_0`，
`--ns` 自定义；`_` 开头保留给宿主）。URL 和 token 在命令输出的 `http_url` / `token` 里
（`--local` 时也会打到 stderr）：

```
GET  http://127.0.0.1:<port>/<ns>/                      # 这个 run 的完整状态
GET  http://127.0.0.1:<port>/<ns>/state                 # 全部 state
GET  http://127.0.0.1:<port>/<ns>/state/<key>           # 读一个值（也接受 /<ns>/<key> 短形式）
POST http://127.0.0.1:<port>/<ns>/state/<key>           # 改一个值（body 原样或 JSON 标量）
GET  http://127.0.0.1:<port>/<ns>/log                   # 日志尾
```

token 用 `?token=` 或 `Authorization: Bearer` 传；没有/错误 → 401。它**不是**文件服务器：
虚拟根里的文件不会因为它在虚拟根里就能被 HTTP 读到。

### 宿主自省：`/_mol/…`（脚本此刻在干啥）

因为每个宿主调用都是 C 函数、Lua 也不是 JIT，运行态可以看得很细：

```
GET /_mol/ping                    -> {"ok":true,"runs":N}
GET /_mol/scripts                 -> 本 service 上所有 run（跨实例）
GET /_mol/scripts/<ns>            -> 完整状态：当前 op、Lua 行号、浅栈、计数、日志、state
GET /_mol/scripts/<ns>/log        -> 日志尾
```

`op` 是正在进行的宿主调用（`net.get https://…`、`proc.run setup.exe`、`fs.write a/b.txt`…），
带 `op_since_ms` 和 `op_timeout_ms`——「卡在哪个网络请求上、还能忍多久」一眼可辨。
`lua_line` 不动而 `instructions` 在涨 = 死循环；`frames` 给出卡在哪层调用里。

## 多实例共用一个端口

`mo-linux serve`（dockerd 形态）持有唯一端口和**全局 namespace 注册表**：所有实例的 `script run`
都提交给它，`_mol/scripts` 一处看全。namespace 全局唯一（重名自动加 `-2`/`-3` 后缀，
`_` 开头保留），所以同一个端口上跑多少个实例都不会串。
`script run` 连不上 service 时会自动拉起一个（exec 自己 `serve --detach`），不需要手动管理生命周期；
空闲 600 秒（`serve --idle-timeout`）自动退出，`serve --stop` 立即退出。

## 边界与承诺

- **实例写 API 与 CLI 同权**：`instance.create` / `mods.*` / `farm.apply` 做的事就是
  `instance init` / `mods install` / `apply` 做的事（同样的校验、同样的布局规则、同样的农场忙检查），
  没有绕过 CLI 的暗门；`--dry-run` 下一律拒绝。
- **不防「用户显式运行的恶意脚本」**：它就是以用户身份跑的；沙箱提高的是误操作与天真恶意脚本的门槛。
- **老内核（< 5.13）不跑 exe**：没有 Landlock 就拒绝，不会假装安全（结果里 `landlock=unavailable`）。
- **exe 能读系统**：它读不到的好处没有（它本就是用户身份），但写不出去。
- **顺序确定**：`fs.list` 排序返回；state 按键排序序列化；不要依赖 `pairs` 顺序或随机数。
## 测试

- `tests/test_lua_script.cpp`（`test_lua_script`）：路径文法、PE 检测、namespace 分配、
  虚拟根读写与逃逸拒绝、符号链接、写入/操作数/指令/内存/墙钟限额、state、自省（死循环行号不动）、
  dry-run、`proc.run` 校验、HTTP server（token / namespace / `_mol` / POST 改 state）、注册表唯一性。
- `tests/check_script_cli.py`（离线 CLI 联调，需隔离 HOME 与 XDG_RUNTIME_DIR）：
  `script run --local --dry-run` 跑示例脚本、逃逸被拒、`serve` 后台模式下 namespace/自省/HTTP state、
  `--no-instance` 从零建实例装 mod 部署、dry-run 拒绝实例写。
  运行：`HOME=/tmp/x XDG_RUNTIME_DIR=/tmp/y python3 tests/check_script_cli.py /path/to/mo-linux`。
