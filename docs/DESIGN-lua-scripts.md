# 提案：Lua 安装脚本层（execute lua scripts）

状态：**提案 + 首版实现说明**。只读体检层（rules）见 [`LUA-RULES.md`](./LUA-RULES.md)，已落地接口见
[`LUA-SCRIPTS.md`](./LUA-SCRIPTS.md)。本文记录安全模型、已做的取舍与后续阶段；**下文「拟定」小节不等于已实现**。
动机：2026-10-10 用户希望 mo-linux 能执行用户编写/下载的 Lua 脚本，把「下载 → 解包 → 跑安装器 → 装成 mod →
部署」复现成一条可审查、可重跑的自动化流程，而不是每次都在 C++ 里加特例。

## 1. 目标与边界

脚本能做什么：在**虚拟根目录**（下称 vroot）里做文件 IO、解包归档、下载、运行 **Windows exe**（wine/proton，
cwd = vroot）、维护一份可被 HTTP 查看/修改的状态。首版**不**开放任何写实例的 API（不装 mod、不改 modlist、
不 apply 农场）——安装动作留给后续带预览与确认的 API，或直接走 CLI。脚本只负责 staging，边界因此非常干净：

> **首版承诺：一个脚本及其拉起的 exe，只能写「虚拟根目录」和它自己跑安装器所必需的 Wine 前缀；**
> **读可以读系统（exe 需要读 prefix/游戏数据/系统库），但 Lua 侧本身只能看见 vroot。**

明确不做：

- 不自动执行 mod 压缩包、集合清单里夹带的 Lua（那是「藏毒」入口，只有用户显式 `script run` 的文件才会跑）。
- 不给 `io` / `os` / `package` / `debug` / `load` / `require` / 原生模块。
- 不给「拼一段 shell 字符串执行」。exe 以 argv 直传，没有 shell。
- 不把 vroot 外的任何路径交给 Lua 拼。

## 2. 安全模型

### 2.1 威胁模型

| 对象 | 防什么 |
|---|---|
| 有 bug 的脚本 | 写错路径毁掉游戏目录/实例；死循环；内存炸弹；解压出一个 100 GB 的包 |
| 天真的恶意脚本（Nexus 教程里抄来的） | 读 `~/.ssh`、外传、跑 `/bin/sh`、改 mo-linux 自己的文件 |
| 恶意/被投毒的 Windows exe（Skyrim 藏毒的主战场） | 往 vroot 外写文件、改游戏目录、往 prefix 里塞东西、读取用户敏感文件、起子进程长期驻留 |
| 其它本地进程/用户 | 未经授权读写脚本的 HTTP namespace |

不防：用户**显式**运行的恶意脚本——它就是以用户身份跑的，Lua 沙箱只是提高门槛，不是权限边界；
内核/wine 0day；需要用户凭据的操作。

### 2.2 Lua 层

- Lua 5.4 ABI 固定（与 rules 层一致，vendored sol2 v3.3.0 + 系统 lua5.4）。
- `lua_newstate` + 自定义分配器（内存上限）+ 计数 hook（指令预算）+ **墙钟截止**（hook 里检查
  `steady_clock`，原生 C 函数耗时长也逃不掉，因为每次 host 调用前后都检查）。
- 只注册宿主表：`host` / `log` / `fs` / `archive` / `net` / `proc` / `state`。没有 `io/os/package/debug/load/
  require/pcall/dofile`，`string` 只留 `find/sub/len/byte/rep/upper/lower/format` 的安全子集
  （`match/gmatch/gsub/dump` 会在原生 C 里跑，hook 断不掉，一律不开；与 rules 层同一理由）。
- 每条脚本独立 state；失败只折叠成一条结构化错误，不影响别的脚本。

### 2.3 虚拟文件系统（vroot）

- **路径文法**（纯函数，可单测）：相对路径、`/` 分隔、首组件不能是 `..`、任何组件不能是 `.`/`..`、
  不允许 `\`、`:`、NUL、控制字符；单组件 ≤ 255 字节、总长 ≤ 4096；不允许绝对路径。
- **解析用 fd，不用路径串**：逐组件 `openat(..., O_PATH|O_NOFOLLOW|O_DIRECTORY)` 向下走，
  最终操作 `openat/mkdirat/unlinkat/renameat/fstatat(..., AT_SYMLINK_NOFOLLOW)`。
  - 符号链接**一律不跟随**（脚本创建不了链接；归档解出的链接会被拒绝；exe 在 vroot 里造的链接也指不出去）；
  - 不依赖 `weakly_canonical` 做事后校验（TOCTOU 竞态下不可靠）；
  - 内核 ≥ 5.6 有 `openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS)`，语义等价，本实现用可移植的
    `O_NOFOLLOW` 逐组件走法（老内核也能跑，行为一致）。
- **硬链接逃逸**：exe 可以把 `/etc/passwd` 这样的文件 `link()` 进 vroot（它本来就能读），
  然后脚本 `fs.read` 读到它。这不是提权（exe 自己就能读），但为了不把「脚本只能看 vroot」变成谎言，
  Landlock 规则里对 `/` 只授只读权、**不给 `REFER`**（`LANDLOCK_ACCESS_FS_REFER`，ABI v1），
  只有 vroot 内允许链接/改名。
- **上限**：单文件读写字节数、vroot 总写入字节数（写累加 + 解包前后 `du`）、fs 操作次数、
  目录列表条数。全部计入 `Limits`，超出即脚本失败（`ok=false` + 可操作错误），不会静默截断。

### 2.4 进程层：只跑 Windows exe

- 只接受 `.exe`（大小写不敏感）后缀、位于 vroot 内、**PE 头可解析**的文件。ELF/脚本/目录一律拒——
  Lua 没有任何办法拉起原生 Linux 程序。
- **PE 机器类型检测**（纯函数）：读 DOS头 → `e_lfanew` → PE 头 → `Machine`：
  `0x14c`=i386、`0x8664`=amd64、`0xaa64`=arm64，其它/损坏 → `unknown`（拒绝运行）。
  检测结果进结果与日志，并决定下面的注入选择。
- **cwd = vroot**（或 vroot 子目录），stdout/stderr 落盘到 `vroot/.mol-logs/proc-N.log`（有界）。
- **超时**：每次 `proc.run` 有墙钟上限（默认 15 分钟），到点杀整个进程组（`setsid` + `kill(-pgid)`），
  不当僵留。
- **argv/env**：argv 每项 ≤ 4096 字节、≤ 64 项、无 NUL；env 只允许 `[A-Za-z_][A-Za-z0-9_]*` 键，
  且**拒绝** `LD_*` / `MOL_COW_*` / `WINEPREFIX` / `WINELOADER*` / `WINEDLL*`（否则脚本能摘掉注入）。
  宿主的 COW/prefix 变量在 env 应用顺序上排在脚本 env **之后**，双保险。

### 2.5 防逃逸：为什么不能靠 hook，32 位怎么办

用户问：怎么给 32 位程序也注入 hook 防逃逸。结论是**不要以 hook 为主**，理由是实测过的机制问题：

- `LD_PRELOAD` 的 `.so` 只能由**同宽度**的动态链接器加载。64 位 `.so` 进不了 32 位进程。
- WoW64 下（64 位 wine 跑 32 位 exe）更糟：32 位 exe 的文件 I/O 由 **32 位 ntdll 直接发
  `int 0x80`/32 位 syscall**，根本不经过 64 位 libc，64 位 preload 库**看不见**这些调用。
  也就是说：`libmol-cow.so` 的写时复制对 32 位 exe **静默失效**——写入会穿过链接改到原文件。
  这对 `mo-linux run` 同样成立（历史隐患，见下）。
- 唯一与位数无关、内核强制、且**被子进程继承**的机制是 LSM：
  - **Landlock**（内核 ≥ 5.13，非特权）：对 exe 进程树（含它派生的 wineserver）施加文件系统规则，
    32/64 位一视同仁；
  - **seccomp**：过滤器装在后代进程上，`int 0x80` 也在过滤范围内（按 `AUDIT_ARCH_I386` 匹配）。

因此 `proc.run` 的 containment 以 Landlock 为主（规则见下），`LD_PRELOAD` 注入降级为
**64 位 exe 的写时复制附加层**，并且：

- 检测到 exe 是 i386 时，结果里明确记一条告警：COW 保护不可用，本次 exe 的写入由 Landlock 兜底；
  若内核没有 Landlock，则拒绝运行 32 位 exe（宁可失败也不静默失保护）。
- CMake 额外编一份 **32 位 `libmol-cow32.so`**（`-m32`，工具链支持时才编），服务**纯 32 位前缀**
  （wine32：loader 本身就是 32 位 ELF，此时 preload 生效）。WoW64 场景它用不上，但纯 32 位前缀下
  32 位工具（一批老行为/生成工具）的保护由此恢复。找不到 32 位库时同样走上面的告警/拒绝逻辑。

Landlock 规则集（ABI v1，`no_new_privs` 后施加，不可撤销、后代继承）：

| 路径 | 授予 |
|---|---|
| `/`（整个文件系统） | 只读：`READ_FILE` `EXECUTE` `READ_DIR` |
| vroot | 上述全部 + 写/建/删/改名/`REFER` |
| 实例的 Wine prefix | 同上全权（安装器要写注册表外的文件、装 VC runtime；`run` 本来就允许） |

即：**exe 能读系统、只能写 vroot 和 prefix**。它读不到的好处没有（它本就是用户身份），
但写不出去——投毒 exe 想往 `~/.config`、游戏目录、mo-linux 目录落文件会直接 `EACCES`。

已知缺口（写进文档，不装作没有）：

- **网络**：v1 不给 exe 断网（seccomp 断 socket 会影响一批需要联网的安装器；Landlock ABI v4 的
  `LANDLOCK_ACCESS_NETWORK_*` 要内核 6.7+）。需要时按脚本开关 + seccomp/netns 收紧，见 WP4。
- 注册表写入走 wineserver，Landlock 管不到前缀内注册表（prefix 本来就可写，见上表）。
- 老内核（< 5.13）没有 Landlock：此时**拒绝**跑 exe（结果里给 `landlock=unavailable` 与可操作提示），
  不做「假装安全」。

### 2.6 网络层

用户已明确：允许脚本下载「乱七八糟的东西」（反正 exe 执行被严格限制）。因此：

- 只允许 `http`/`https` scheme（`file:`、`gopher:`、重定向到非 http(s) 一律拒——libcurl 侧也锁
  `CURLOPT_PROTOCOLS`）。
- 上限：单次响应体字节数、下载总字节数、超时、请求次数；响应体先进内存再交给 Lua（有界），
  下载直接落 vroot（`.part` + 原子 rename，复用 `http_download`）。
- **默认允许**回环/私网/链路本地（用户允许下载任意东西，也允许打内网地址）；提供
  `--deny-private` 开关把这些段（loopback、RFC1918、link-local、CGNAT、组播、保留段、IPv6 ULA、
  IPv4-mapped）加入黑名单，给谨慎的用户。
- HTTP **服务端**只绑 `127.0.0.1`，每个 run 一个随机 token（`Authorization: Bearer` 或 `?token=`），
  防止同机其它用户/进程读写脚本状态。

### 2.7 通信：HTTP namespace，不做裸 socket

用户给了两个选项。选 **HTTP namespace**，理由：

- 裸 socket 意味着脚本可以连任意主机任意端口、也可以监听端口——策略、认证、可观测性全部失控，
  要安全就得先有 netns + seccomp + Landlock ABI v4（WP4 的事）。
- HTTP namespace 把能力切成「查看看状态 / 改状态」两件事，路径即 key，天然可审计；
  用户浏览器直接可看，GUI 也好接。

**保留路径**：所有以 `_` 开头的路径归宿主（introspection，见 §3）。用户 namespace 文法
`[a-z0-9][a-z0-9_-]{0,31}`，默认 `script_0`，可 `--ns` 指定；同一 server 内重复注册会被拒
（自动加后缀 `-2`、`-3`…，绝不串用）。

### 2.8 多实例共用一个端口：常驻 service（dockerd 形态）

用户问：所有实例能不能共用一个端口、namespace 不串。能——**用一个常驻的 `mo-linux serve` 守护进程
持有唯一端口和全局 namespace 注册表**（dockerd 形态，而不是「每个 CLI 各起各的」）：

```
mo-linux serve [--port 27736] [--detach]      # 守护进程：TCP(127.0.0.1) + unix socket + 注册表
mo-linux script run install.lua               # 自动连上守护进程（没有就拉起来），提交 run
```

- **一个端口**：守护进程 `bind(127.0.0.1:27736)`；HTTP（脚本 namespace + `_mol` introspection）由它统一提供。
- **namespace 不串**：注册表在守护进程里，`script_0`、`script_1`… 全局唯一；重名自动加后缀。
  `_` 开头保留给宿主（`_mol`）。用户 `--ns skyrim-main` 指定自己的名字，撞了同样自动加后缀。
- **跨实例一处看全**：`GET /_mol/scripts` 列出所有实例的所有 run（含当前 op、Lua 行号、计数）。
- **CLI 只是客户端**：`script run` 把「脚本路径 + 实例 + 选项」通过 unix socket 发给守护进程，
  然后流式收事件（log / op 变化 / 终态）。CLI 退出不影响 run 继续（长安装挂后台，随时可看）。
- **发现与防重**：socket 落在 `$XDG_RUNTIME_DIR/mo-linux-<uid>/serve.sock`（目录 0700、socket 0600），
  同目录 `serve.lock` 用 `flock` 保证只有一个守护进程；`serve.json` 记录 pid/port 供发现。
  自动拉起：`script run` 连不上就 `exec` 自己一次 `serve --detach` 并等 socket 就绪（幂等，抢锁失败
  说明别人已经拉起来了）。
- **生命周期**：守护进程在「无 run 且无连接」空闲 `--idle-timeout`（默认 600s）后自行退出；
  `serve` 显式启动（非 `--detach）时前台运行，适合 systemd `socket activation` 用户。
- **`--local`**：`script run --local` 不进守护进程，当前进程内直接跑（测试、一次性、或不想留后台时用）。
  两种模式跑的是同一份 `Run` 实现，只是驱动方不同。

### 2.9 为什么不用「.so 里的共有字段」共享状态

用户问：是不是该用 so 的共有字段（common section / 共享页）在进程间共享注册表。**不建议**，理由：

- `.so` 的 common/bss 段共享依赖「两个进程 map 同一文件且用 `MAP_SHARED`」——动态链接器默认
  `MAP_PRIVATE`，要共享得显式 `mmap` 一个具名段，那已经是「共享内存」而不是「so 字段」了；
  靠链接期布局做 IPC，重编译/换 ABI/部分更新就会静默错位，没有版本协商余地。
- 共享内存本身不带同步原语：注册表要「分配唯一 ns」这种临界区，得自己在 shm 里实现锁，
  进程崩溃在锁内就成了死锁，恢复逻辑比协议本身还难写。
- 守护进程把状态变成**单拥有者**：唯一性、取消、`_mol` 汇总、空闲退出都是一段普通代码，
  unix socket 有权限（0600）和连接语义，崩了最多重启，不存在「半个注册表」的中间态。
   dockerd 就是这么干的：状态在守护进程里，客户端只发请求。

如果将来要做「无守护进程的一次性共享」（比如两个 CLI 偶遇），用 `flock` 文件 + `SCM_RIGHTS`
传 listen fd 也比 shm 干净；但那不在本期范围。

## 3. 宿主 introspection namespace（`_mol`）

用户要求：C++ 侧提供一个「现在这个脚本在干啥」的查看方式。因为 Lua 不是 JIT、每个宿主调用都是 C 函数，
这件事可以做得比堆栈转储更友好：

- **当前操作**：每次 host 调用进入时记录 `{name, detail, since}`（`fs.write 'a/b.txt'`、
  `net.get 'https://…'`、`proc.run 'setup.exe'`、`archive.extract …`），退出时清空。
  HTTP 里能看到「卡在 `net.get https://cdn…` 已经 4 分钟」——超时是多少、还能忍多久都写出来。
- **当前 Lua 行**：计数 hook 每次触发用 `lua_getinfo(L, "l")` 记 `source:currentline`
  （纯读，不分配）。死循环 = 行号不动 + 指令数在涨，一眼可辨。
- **浅栈**：每 100 次 hook（10 万指令）用 `lua_getstack/getinfo` 抓 ≤ 16 帧的
  `函数名:行号` 快照（不分配 Lua 堆），给出「卡在哪层调用里」。
- 计数：fs_ops / proc_runs / net_requests / instructions / vroot 写入字节。
- 日志尾、state 快照。

端点（token 同 §2.6）：

```
GET /_mol/ping                      -> {"ok":true,"server":"mo-linux script host","version":…}
GET /_mol/scripts                   -> 本 server 上所有脚本 run：[{ns, script, state, op, since_ms, …}]
GET /_mol/scripts/<ns>              -> 单个 run 的完整状态（op/lua/counters/log/state）
GET /_mol/scripts/<ns>/log         -> 日志尾（?lines=N）
GET /<ns>/                          -> 脚本自己的 namespace：state + 日志尾
GET /<ns>/state/<key>               -> 读一个状态值
POST /<ns>/state/<key>  (body=值)   -> 改一个状态值（脚本侧 state.get 立即可见）
```

`state` 是扁平的 `key → 标量`（string/number/boolean），key 文法 `[a-z0-9][a-z0-9._/-]{0,127}`
（用户例子里的 `http.get("/xxx/xxx")` 就是这种路径型 key）。HTTP 侧**不是**文件服务器——
vroot 里的文件不会因为它在 vroot 里就能被 HTTP 读到。

## 4. 脚本 API（v1，全部走 §2 的沙箱）

```lua
host.name()            -- 'mo-linux'
log.info(msg)          -- msg ≤ 4 KiB；进结果与 HTTP 日志（≤ limits.log_lines 条）

fs.read(path) -> string|nil          -- ≤ limits.file_bytes
fs.write(path, data) -> true
fs.append(path, data) -> true
fs.exists(path) -> bool
fs.size(path) -> number|nil
fs.list(path) -> {names...}|nil      -- 只有直接子项，排序，≤ 4096 项
fs.mkdir(path) -> true               -- mkdir -p
fs.remove(path) -> true              -- 递归删（只在 vroot 内）
fs.copy(src, dst) -> true
fs.move(src, dst) -> true

archive.list(path) -> {entries...}   -- 归档内文件路径，≤ 20000 项
archive.extract(path, dest) -> true  -- dest 在 vroot 内；解完拒绝符号链接/越界条目

net.get(url, {headers=…, timeout_ms=…}) -> {status=…, body=…}
net.download(url, path) -> bytes     -- path 在 vroot 内

proc.run(exe, {args={…}, env={…}, timeout_ms=…, cwd=…}) -> {exit=…, log=…, arch=…}
                                      -- exe 必须 .exe + PE + vroot 内；cwd 默认 vroot 根

state.get(key, default) -> value
state.set(key, value) -> true
state.delete(key) -> true
state.keys() -> {keys...}
```

错误处理：宿主调用失败 = Lua 错误（`error()` 语义），带可操作信息；脚本自己 `pcall` 是不存在的
（防用 pcall 吞掉预算异常），所以**写脚本要当作「失败即停」**。这一点在文档里写明。

## 5. 限额（`Limits`）

| 项 | 默认 | 超出后 |
|---|---|---|
| `source_bytes` 脚本大小 | 256 KiB | 拒载 |
| `memory_bytes` Lua 堆 | 32 MiB（夹到 [1 MiB, 128 MiB]） | 分配失败 → 脚本失败 |
| `instructions` 指令预算 | 4e9 | hook 触发 → 脚本失败 |
| `wall_ms` 整脚本墙钟 | 60 min | 下一次 hook/宿主调用 → 脚本失败 |
| `file_bytes` 单文件读写 | 2 GiB | 该次调用失败 |
| `vroot_bytes` vroot 总写入 | 16 GiB | 写入类调用失败 |
| `fs_ops` fs 调用次数 | 100k | 调用失败 |
| `proc_runs` exe 次数 | 32 | 调用失败 |
| `proc_timeout_ms` 单个 exe | 15 min | 杀进程组 → 该次调用失败 |
| `net_requests` 请求次数 | 512 | 调用失败 |
| `net_body_bytes` 单次响应体 | 256 MiB | 调用失败 |
| `state_entries` / `state_value_bytes` | 4096 / 256 KiB | 调用失败 |
| `log_lines` | 2000 | 丢弃更早的行 |

## 6. 分阶段交付与验收

### WP1（本次，已落地）：安全核心 + HTTP namespace + introspection + 常驻 service

`mol::script` 模块（`core/src/lua_script*.cpp`）：Lua 沙箱、vroot fd 式文件系统、PE 检测、proc.run
（Landlock 收容 + 超时杀组；不注入 LD_PRELOAD，理由见 §2.5）、net（scheme/上限/可选私网黑名单）、state、
HTTP server（`_mol` introspection + 每脚本 namespace + token）、常驻 `serve` 守护进程
（unix socket 协议 + 注册表 + 自动拉起 + 空闲退出）、CLI `script run` / `serve`。

验收（`tests/test_lua_script.cpp` + `tests/check_script_cli.py`，全离线）——全部通过：

1. 路径文法：绝对路径、`..`、`\`、盘符、NUL、超长、空——全部拒绝，且拒绝发生在**任何 IO 之前**。
2. 符号链接逃逸：vroot 里造一个指向 `/etc` 的链接，`fs.read`/`fs.list`/`fs.remove` 全部失败。
3. 预算：`while true do end`、内存炸弹、写超 `file_bytes`/`vroot_bytes`、超过 `fs_ops`——都变成
   `ok=false` 的结构化错误，不挂死不 OOM。
4. PE 检测：夹具字节（i386/amd64/arm64/坏头/ELF）→ 正确机器类型；非 `.exe`、vroot 外、非 PE
   一律拒绝运行。
5. HTTP：起 server，token 错误 401；`POST /<ns>/state/k` 后 `state.get` 立即可见；
   `_mol/scripts/<ns>` 能看到当前 op 与行号（用一个自旋脚本验证「行号不动、指令数在涨」）。
6. 单端口多实例：两个不同实例各 `script run`，经同一个守护进程，**同一个端口**，
   namespace 分别是 `script_0`/`script_1`（或用户指定），`GET /_mol/scripts` 同时列出两个；
   重名不串（自动加后缀）。
7. CLI：`script run` 跑 `scripts/example-stage.lua`（只 fs/state，不联网不跑 exe），
   结果含 vroot 路径、HTTP URL、日志；`--dry-run` 用临时 vroot，不碰实例；
   `--local` 不进守护进程。

**实现期踩到并修掉的两个真 bug**（记下来避免复发）：

- 本 sol2 构建下 `table::set(key, lambda)` / `create_table_with(k, lambda)` 会把闭包**绑到错误的函数**
  （调用方拿到的是别表的函数；`table_proxy::operator=` 内部走的 `set_function` 才对）。
  现在所有宿主函数一律用 `set_function` 注册。
- `std::string_view kv = r.query.substr(...)` 把视图绑到 `substr` 的**临时 string** 上 → 悬空
  （token 校验因此永远失败）。必须 `std::string_view(r.query).substr(...)`。

### WP2（已落地）：实例写 API + 从零建实例

`instance.create` / `instance.info` / `mods.install_archive` / `mods.install_staged`
（vroot 目录 → `mods/<name>`，布局判定复用 `install_archive` 的规则，新增
`install_directory` primitive）/ `mods.enable|disable|list` / `farm.apply`。
`--no-instance` 让脚本在没有实例时也能起跑，于是**一个脚本可以从零装一个游戏**：
建实例 → 下载/解包 → 跑安装器 exe → `install_staged` → `farm.apply`。

与 CLI 同权同校验（没有暗门）：`game_dir` 必须已存在、路径必须绝对、runner 只能是 proton/wine、
农场忙（`require_farm_idle`）照抄 `run` 的保护、`--dry-run` 一律拒绝。
同一实例的并发写按实例根串行（守护进程可能同时跑多个脚本）。

**实现期踩到的坑**（第三条是安全漏洞，已修）：

- `InitOptions` 持 `std::string_view`：`io.root = base + "/instance"` 绑到临时 string 上 → 悬空
  （实例建到乱码路径）。必须先用局部 `std::string` 承接。
- `std::filesystem::copy(recursive)` **会跟随符号链接**：`install_staged` 因此可能把 vroot 外的文件
  内容复制进 mod。已在复制前对源树做 `validate_tree`（拒绝符号链接与越界条目），复制后再校验一次。
- `fs.list()` 曾把 ENOTDIR（符号链接/非目录）当成「不存在」返回 nil，与「不跟随链接」的承诺不一致；
  现在只有 ENOENT 返回 nil。

### WP3：exe 网络收紧（可选）

`proc.run` 的 `net=false` 选项 + seccomp 断 `socket/connect`（32/64 位通用，按 `AUDIT_ARCH_I386`
也过滤 `int 0x80`），或 Landlock ABI v4（内核 ≥ 6.7）+ 私有无网络命名空间。默认仍放行
（安装器要联网），需要时按脚本/按 exe 打开。

### WP4：取消与跨实例复用

守护进程加 `cancel`（杀 run 线程 + 其 exe 进程组，vroot 保留供检查）；
`script run --reuse <ns>` 复用同一 vroot 续跑；`_mol` 汇总跨实例的磁盘/下载缓存视图。

## 7. 与 rules 层的关系

同一个 Lua 5.4 + sol2 + 有界 state 的底座，两个用途：

- **rules**：只读体检（`doctor`/`next`），无 IO 无进程无网络，输出诊断与 `disable_mod` 动作。
- **scripts**：安装自动化，有 IO/进程/网络，但被关进 vroot + Landlock + 限额里。

两层共享 `Limits` 的思路与错误隔离语义，不共享状态：规则不会因为某个脚本在跑而结果不同
（脚本运行期不产生 facts）。合集中的 Lua 永远不进 rules 目录、也不会被自动执行。
