#pragma once
// Lua 安装脚本：沙箱 + 虚拟根（vroot）+ Windows exe 执行 + HTTP namespace + 常驻 service。
// 安全模型与取舍见 docs/DESIGN-lua-scripts.md；已落地接口见 docs/LUA-SCRIPTS.md。
//
// 本模块只用文本形式的标准库头 + sol2 + 系统 lua5.4（不 import alib6/std 模块，
// 混用约束见 cli/cmd_common.hpp 文件头）。JSON 用模块内手写的最小编解码，够协议用即可。
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mol/instance.hpp"
#include "mol/pmr.hpp"

namespace mol::script {

// ---------------------------------------------------------------------------
// 纯函数（单测直接打；不碰文件系统/网络）
// ---------------------------------------------------------------------------

// vroot 内相对路径文法：非空、≤4096 字节、'/' 分隔、组件非空且不是 "."/".."、组件 ≤255 字节，
// 拒绝 '\\'、':'、NUL、其它控制字符、绝对路径、尾部 '/'。
bool valid_vpath(std::string_view p);

// PE 头机器类型："i386"(0x14c) | "amd64"(0x8664) | "arm64"(0xaa64) | "unknown"。
// 打不开/不是 MZ+PE/头损坏/截断都算 unknown（调用方据此拒绝运行）。
std::string pe_machine(std::string_view host_path);

// 只放行 http/https（scheme 大小写不敏感）；拒绝 file/gopher/ftp/…、空白与控制字符、userinfo、空 host。
bool http_url_ok(std::string_view url);

// --deny-private 用：主机名（字面量 IP 或可解析域名）是否落在回环/私网/链路本地/保留段。
// 解析失败返回 false（放行；网络错误不该被当成安全事件）。
bool host_is_private(std::string_view host);

// namespace 文法：[a-z0-9][a-z0-9_-]{0,31}；'_' 开头保留给宿主（_mol），一律拒绝。
bool valid_ns(std::string_view ns);

// state key 文法：[a-z0-9][a-z0-9._/-]{0,127}；组件不能是 "."/".."。
bool valid_state_key(std::string_view key);

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
struct Limits {
    std::size_t source_bytes = 256 * 1024;        // 脚本源码大小
    std::size_t memory_bytes = 32 * 1024 * 1024;  // Lua 堆（夹到 [1 MiB, 128 MiB]）
    std::uint64_t instructions = 4'000'000'000ull; // 指令预算（0 = 交给下限 1e6）
    std::uint64_t wall_ms = 60ull * 60 * 1000;    // 整脚本墙钟
    std::uint64_t file_bytes = 2ull * 1024 * 1024 * 1024;  // 单文件读/写
    std::uint64_t vroot_bytes = 16ull * 1024 * 1024 * 1024; // vroot 总写入
    std::size_t fs_ops = 100'000;                 // fs.* 调用次数
    std::size_t proc_runs = 32;                   // proc.run 次数
    std::uint64_t proc_timeout_ms = 15ull * 60 * 1000;  // 单个 exe 墙钟
    std::size_t net_requests = 512;               // net.* 次数
    std::uint64_t net_body_bytes = 256ull * 1024 * 1024; // 单次响应体
    std::size_t state_entries = 4096;             // state 键数
    std::size_t state_value_bytes = 256 * 1024;   // 单个 state 值
    std::size_t log_lines = 2000;                 // 日志行数（丢最旧）
};

struct Options {
    Limits limits;
    std::string ns;                // 空 = 自动分配 script_<n>
    bool net = true;               // 允许 net.*（dry_run 下强制 false）
    bool deny_private = false;     // 额外拒绝回环/私网/链路本地
    bool dry_run = false;          // vroot 换成临时目录；proc/net 禁用
    std::string root;              // 空 = <instance>/scripts/<脚本名>.work（dry_run 下 <tmp>/…）
    std::string http_url, token;   // 调用方（HTTP server 拥有者）在 start() 前填好；进 Status/RunResult
};

// ---------------------------------------------------------------------------
// 运行期状态（HTTP introspection 的数据源；脚本线程写、HTTP/客户端线程读，内部加锁）
// ---------------------------------------------------------------------------
struct OpInfo {
    std::string name, detail;            // name 空 = 正在跑纯 Lua（无宿主调用）
    std::int64_t since_ms = 0;           // steady_clock 毫秒
    std::int64_t timeout_ms = 0;         // 0 = 无超时
};
struct Status {
    std::string ns, script, instance, root, http_url, token;
    std::string run_state = "running";   // running | done | failed
    OpInfo op;
    std::string lua_source;              // chunk 名（脚本文件名）
    int lua_line = 0;                    // 最近一次 hook 看到的行（死循环 = 不动）
    std::vector<std::string> frames;     // 浅栈快照 "name:line"（≤16 帧）
    std::uint64_t instructions = 0, fs_ops = 0, proc_runs = 0, net_requests = 0, bytes_written = 0;
    std::vector<std::string> log;
    std::map<std::string, std::string> state;
    std::string error, landlock;         // landlock: "v1" | "unavailable" | ""
    std::int64_t started_ms = 0, finished_ms = 0;
};

struct RunResult {
    bool ok = false;
    std::string error;
    std::string ns, root, http_url, token, landlock;
    std::vector<std::string> log;
    std::map<std::string, std::string> state;
    std::uint64_t fs_ops = 0, proc_runs = 0, net_requests = 0;
};

// 事件：一行 NDJSON（daemon 模式下原样穿过 unix socket）。至少会出现
// {"event":"started"…} / {"event":"log","line":…} / {"event":"op",…} / {"event":"done",…}。
using EventFn = std::function<void(const std::string& line)>;

// ---------------------------------------------------------------------------
// Run：本地（线程里跑沙箱）或远端（守护进程里跑，事件流回来）两种实现
// ---------------------------------------------------------------------------
class Run {
  public:
    virtual ~Run() = default;
    [[nodiscard]] virtual std::string ns() const = 0;
    [[nodiscard]] virtual Status status() const = 0;
    [[nodiscard]] virtual RunResult result() const = 0;
    virtual void join() = 0;
    virtual bool done() const = 0;
    // 开始执行。LocalRun 起线程（vroot 定位失败会抛给调用方）；RemoteRun 已在守护进程里跑起来，空实现。
    virtual void start() {}
    // HTTP namespace 的 POST：改脚本可见的 state。返回 false = 已结束或拒绝（值不合法/超限）。
    // 本地实现直接写沙箱的 state；远端实现由守护进程代写（未接线时返回 false）。
    virtual bool set_state(const std::string& key, const std::string& value) { (void)key; (void)value; return false; }
};
using RunPtr = std::shared_ptr<Run>;

// 本地执行。script = 绝对路径（只作展示），text = 源码，instance_dir 空 = 无实例（proc.run 不可用）。
// ns 必须已经是唯一 namespace（见 unique_ns / Registry::add）；event 可为空。
RunPtr local_run(std::string script, std::string text, std::string instance_dir, Options opt, std::string ns,
                 EventFn event = {});

// 分配唯一 namespace：want 空 → script_<n>（n 从 0 找第一个空位）；合法但已占用 → 追加 -2/-3…；
// 非法（文法不符、'_' 开头）→ 具名报错。纯函数，单测直接打。
std::string unique_ns(const std::vector<std::string>& taken, std::string_view want);

// ---------------------------------------------------------------------------
// 注册表：namespace 唯一性（守护进程与 local 模式共用同一套契约）
// ---------------------------------------------------------------------------
class Registry {
  public:
    virtual ~Registry() = default;
    // 登记一个 run（ns 必须已通过 unique_ns 分配且未占用，否则具名报错）。
    virtual void add(const RunPtr& run) = 0;
    virtual void remove(std::string_view ns) = 0;
    [[nodiscard]] virtual std::vector<RunPtr> runs() const = 0;
    [[nodiscard]] virtual RunPtr find(std::string_view ns) const = 0;
    [[nodiscard]] virtual std::size_t size() const = 0;
};
using RegistryPtr = std::shared_ptr<Registry>;
RegistryPtr make_registry();

// ---------------------------------------------------------------------------
// HTTP server：127.0.0.1 + token；/_mol/… 宿主自省，/<ns>/… 脚本 namespace
// ---------------------------------------------------------------------------
class HttpServer {
  public:
    // port 0 = 临时端口；被占用抛 Error{io_error}。token 空 = 自动生成（32 hex）。
    HttpServer(Registry& reg, std::uint16_t port, std::string token = {});
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] const std::string& token() const;
    [[nodiscard]] std::string url_for(std::string_view ns) const;  // http://127.0.0.1:port/<ns>/
    static std::string random_token();
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// 常驻 service（dockerd 形态）：一个端口 + 全局 namespace 注册表 + unix socket 协议
// ---------------------------------------------------------------------------
std::string default_socket_path();  // $XDG_RUNTIME_DIR/mo-linux-<uid>/serve.sock（回退 /tmp）

// 守护进程入口（前台运行；调用方自己处理 --detach 的 double-fork）。
// idle_timeout_s ≤ 0 = 不空闲退出。actual_port 非空时回传实际监听端口。返回进程退出码（0）。
int serve_main(std::uint16_t port, int idle_timeout_s, std::string_view socket_path,
               std::uint16_t* actual_port = nullptr);

// 客户端：能连上就把 run 交给守护进程（实例/选项随请求过去），事件经 event 回调流回；
// 连不上返回 nullptr。script 为绝对路径，text 为源码。
RunPtr daemon_submit(std::string_view socket_path, std::string script, std::string text,
                     std::string instance_dir, const Options& opt, const EventFn& event);
bool daemon_ping(std::string_view socket_path, std::uint16_t* port);

// 连不上就拉起一个守护进程（exec 自己 serve --detach --port preferred_port）再提交；
// 仍失败返回 nullptr。
RunPtr ensure_daemon_submit(std::string_view socket_path, std::string script, std::string text,
                            std::string instance_dir, const Options& opt, const EventFn& event,
                            std::uint16_t preferred_port);

// 默认端口（约定值；被占时守护进程具名报错，CLI 可换 --port）。
constexpr std::uint16_t kDefaultPort = 27736;

}  // namespace mol::script
