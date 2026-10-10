// 常驻 service（dockerd 形态）：一个端口 + 全局 namespace 注册表 + unix socket 协议。
// `mo-linux serve [--detach]` 起守护进程；`mo-linux script run` 连上来提交 run 并流式收事件。
// 状态（注册表、HTTP server、run 线程）全在守护进程里，客户端只发请求——见 docs/DESIGN-lua-scripts.md §2.8。
#include "mol/lua_script.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "lua_script_json.hpp"
#include "lua_script_http.hpp"
#include "mol/error.hpp"

namespace mol::script {
namespace {

using namespace std::chrono;

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop = true; }

std::string uid_tag() {
    const uid_t uid = ::getuid();
    return std::to_string(static_cast<unsigned>(uid));
}

std::string socket_dir() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg)
        return std::string(xdg) + "/mo-linux-" + uid_tag();
    return "/tmp/mo-linux-" + uid_tag();
}

bool write_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// 读一行（NDJSON 协议）。上限 8 MiB（脚本源码随请求一起送）。
std::string read_line(int fd, std::size_t cap) {
    std::string line;
    char buf[8192];
    const auto deadline = steady_clock::now() + seconds(30);
    while (line.find('\n') == std::string::npos) {
        if (line.size() > cap) return {};
        const auto left = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
        if (left <= 0) return {};
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left)) <= 0 || !(p.revents & POLLIN)) return {};
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return {};
        line.append(buf, static_cast<std::size_t>(n));
    }
    line.resize(line.find('\n'));
    while (!line.empty() && (line.back() == '\r')) line.pop_back();
    return line;
}

int connect_unix(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

Options options_from_json(const JValue& o) {
    Options opt;
    if (const JValue* v = o.find("ns")) opt.ns = v->as_string();
    if (const JValue* v = o.find("net")) opt.net = v->as_bool(true);
    if (const JValue* v = o.find("deny_private")) opt.deny_private = v->as_bool(false);
    if (const JValue* v = o.find("dry_run")) opt.dry_run = v->as_bool(false);
    if (const JValue* v = o.find("root")) opt.root = v->as_string();
    if (const JValue* v = o.find("memory_mb")) {
        const double mb = v->t == JValue::T::Num ? v->num : 32;
        opt.limits.memory_bytes = static_cast<std::size_t>(std::clamp(mb, 1.0, 128.0) * 1024 * 1024);
    }
    if (const JValue* v = o.find("wall_minutes")) {
        const double m = v->t == JValue::T::Num ? v->num : 60;
        opt.limits.wall_ms = static_cast<std::uint64_t>(std::clamp(m, 0.1, 24 * 60.0) * 60 * 1000);
    }
    return opt;
}

std::string options_to_json(const Options& o) {
    JValue v;
    v.t = JValue::T::Obj;
    auto add = [&v](const char* k, JValue x) { v.obj.emplace_back(k, std::move(x)); };
    auto s = [](std::string str) {
        JValue x;
        x.t = JValue::T::Str;
        x.str = std::move(str);
        return x;
    };
    auto b = [](bool val) {
        JValue x;
        x.t = JValue::T::Bool;
        x.b = val;
        return x;
    };
    auto n = [](double num) {
        JValue x;
        x.t = JValue::T::Num;
        x.num = num;
        return x;
    };
    add("ns", s(o.ns));
    add("net", b(o.net));
    add("deny_private", b(o.deny_private));
    add("dry_run", b(o.dry_run));
    add("root", s(o.root));
    add("memory_mb", n(static_cast<double>(o.limits.memory_bytes) / (1024 * 1024)));
    add("wall_minutes", n(static_cast<double>(o.limits.wall_ms) / (60 * 1000)));
    return json_dump(v);
}

// ---------------------------------------------------------------------------
// 守护进程
// ---------------------------------------------------------------------------
struct Daemon {
    RegistryPtr reg;
    std::unique_ptr<HttpServer> http;
    std::string socket_path, dir;
    steady_clock::time_point last_activity = steady_clock::now();
    mutable std::mutex activity_mu;

    void touch() {
        std::lock_guard lk(activity_mu);
        last_activity = steady_clock::now();
    }
    [[nodiscard]] bool idle_for(int timeout_s) const {
        if (timeout_s <= 0) return false;
        std::lock_guard lk(activity_mu);
        return reg->size() == 0 && steady_clock::now() - last_activity > seconds(timeout_s);
    }
};

// 处理一条客户端连接：读请求 → 起 run → 流事件 → 结束清理。
void serve_connection(Daemon& d, int fd) {
    d.touch();
    const std::string line = read_line(fd, 8 * 1024 * 1024);
    if (line.empty()) {
        ::close(fd);
        return;
    }
    JValue req;
    if (!json_parse(line, req) || req.t != JValue::T::Obj) {
        write_all(fd, "{\"event\":\"error\",\"error\":\"invalid request\"}\n");
        ::close(fd);
        return;
    }
    const JValue* cmd = req.find("cmd");
    const std::string c = cmd ? cmd->as_string() : "";
    if (c == "ping") {
        write_all(fd, "{\"event\":\"pong\",\"port\":" + std::to_string(d.http->port()) + "}\n");
        ::close(fd);
        return;
    }
    if (c == "shutdown") {
        write_all(fd, "{\"event\":\"bye\"}\n");
        ::close(fd);
        g_stop = true;
        return;
    }
    if (c != "run") {
        write_all(fd, "{\"event\":\"error\",\"error\":\"unknown command\"}\n");
        ::close(fd);
        return;
    }
    const JValue* script = req.find("script");
    const JValue* text = req.find("text");
    if (!script || script->t != JValue::T::Str || !text || text->t != JValue::T::Str) {
        write_all(fd, "{\"event\":\"error\",\"error\":\"run needs script and text\"}\n");
        ::close(fd);
        return;
    }
    const std::string instance = [&] {
        const JValue* i = req.find("instance");
        return i && i->t == JValue::T::Str ? i->as_string() : std::string();
    }();
    Options opt;
    if (const JValue* o = req.find("options"); o && o->t == JValue::T::Obj) opt = options_from_json(*o);

    RunPtr run;
    try {
        std::vector<std::string> taken;
        for (const auto& r : d.reg->runs()) taken.push_back(r->ns());
        const std::string ns = unique_ns(taken, opt.ns);
        auto event = [fd](const std::string& ev) { write_all(fd, ev + "\n"); };
        opt.http_url = d.http->url_for(ns);
        opt.token = d.http->token();
        run = local_run(script->as_string(), text->as_string(), instance, std::move(opt), ns, std::move(event));
        d.reg->add(run);
        run->start();
        const Status st = run->status();
        write_all(fd, event_started(st) + "\n");
    } catch (const std::exception& e) {
        write_all(fd, "{\"event\":\"done\",\"ok\":false,\"error\":" + json_quote(e.what()) + "}\n");
        ::close(fd);
        return;
    }
    // 在当前连接线程里等它结束（run 自己的线程在跑，事件经回调写回 socket）
    run->join();
    d.reg->remove(run->ns());
    d.touch();
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
}

}  // namespace

std::string default_socket_path() { return socket_dir() + "/serve.sock"; }

int serve_main(std::uint16_t port, int idle_timeout_s, std::string_view socket_path_in,
               std::uint16_t* actual_port) {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    auto d = std::make_shared<Daemon>();  // 连接线程按值捕获：退出前它们一定还活着
    d->dir = socket_dir();
    d->socket_path = socket_path_in.empty() ? default_socket_path() : std::string(socket_path_in);
    std::error_code ec;
    std::filesystem::create_directories(d->dir, ec);
    std::filesystem::permissions(d->dir, std::filesystem::perms::owner_all, ec);
    if (ec) {
        std::fprintf(stderr, "serve: cannot create %s: %s\n", d->dir.c_str(), ec.message().c_str());
        return 1;
    }
    // 单实例锁：抢不到说明已经有一个守护进程
    const std::string lock_path = d->dir + "/serve.lock";
    const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "serve: another instance is already running (%s)\n", lock_path.c_str());
        if (lock_fd >= 0) ::close(lock_fd);
        return 1;
    }
    {
        std::ofstream f(lock_path, std::ios::trunc);
        f << ::getpid();
    }
    d->reg = make_registry();
    try {
        d->http = std::make_unique<HttpServer>(*d->reg, port);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "serve: %s\n", e.what());
        ::close(lock_fd);
        return 1;
    }
    {  // 发现文件：pid/port/socket
        std::ofstream f(d->dir + "/serve.json", std::ios::trunc);
        f << "{\"pid\":" << ::getpid() << ",\"port\":" << d->http->port()
          << ",\"socket\":" << json_quote(d->socket_path) << "}";
    }
    ::unlink(d->socket_path.c_str());
    const int sfd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) {
        std::fprintf(stderr, "serve: cannot create the control socket\n");
        return 1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, d->socket_path.c_str(), sizeof addr.sun_path - 1);
    if (::bind(sfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(sfd, 32) != 0) {
        std::fprintf(stderr, "serve: cannot bind %s: %s\n", d->socket_path.c_str(), std::strerror(errno));
        return 1;
    }
    ::chmod(d->socket_path.c_str(), 0600);
    std::fprintf(stderr, "serve: listening on http://127.0.0.1:%u (token via `script run`), control %s\n",
                 d->http->port(), d->socket_path.c_str());
    if (actual_port) *actual_port = d->http->port();
    while (!g_stop) {
        pollfd p{sfd, POLLIN, 0};
        const int rc = ::poll(&p, 1, 200);
        if (rc > 0 && (p.revents & POLLIN)) {
            const int fd = ::accept4(sfd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd >= 0) std::thread([d, fd] { serve_connection(*d, fd); }).detach();
            d->touch();
        }
        if (d->idle_for(idle_timeout_s)) {
            std::fprintf(stderr, "serve: idle for %ds, exiting\n", idle_timeout_s);
            break;
        }
    }
    ::unlink(d->socket_path.c_str());
    ::close(sfd);
    d->http.reset();  // 等在途 HTTP 请求
    d->reg.reset();
    ::close(lock_fd);
    ::unlink(lock_path.c_str());
    std::fprintf(stderr, "serve: stopped\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 客户端
// ---------------------------------------------------------------------------
bool daemon_ping(std::string_view socket_path, std::uint16_t* port) {
    const int fd = connect_unix(socket_path.empty() ? default_socket_path() : std::string(socket_path));
    if (fd < 0) return false;
    write_all(fd, "{\"cmd\":\"ping\"}\n");
    const std::string line = read_line(fd, 64 * 1024);
    ::close(fd);
    if (line.empty()) return false;
    JValue v;
    if (!json_parse(line, v)) return false;
    if (const JValue* p = v.find("port"); p && port && p->t == JValue::T::Num)
        *port = static_cast<std::uint16_t>(p->num);
    return v.find("event") && v.find("event")->as_string() == "pong";
}

namespace {

// 远端 run：守护进程里跑，事件流回来。status() 是事件流的 best-effort 视图。
class RemoteRun : public Run {
  public:
    RemoteRun(int fd, std::string ns, EventFn event) : fd_(fd), ns_(std::move(ns)) {
        st_.run_state = "running";
        if (event) event_ = std::move(event);
        th_ = std::thread([this] { pump(); });
    }
    ~RemoteRun() override { join(); }
    void join() override {
        if (th_.joinable()) th_.join();
    }
    [[nodiscard]] bool done() const override { return done_.load(); }
    [[nodiscard]] std::string ns() const override {
        std::lock_guard lk(mu_);
        return ns_;
    }
    [[nodiscard]] Status status() const override {
        std::lock_guard lk(mu_);
        return st_;
    }
    [[nodiscard]] RunResult result() const override {
        std::lock_guard lk(mu_);
        return res_;
    }
    void start() override {}  // 守护进程侧已经在跑：构造时就开始泵事件流

    void pump();  // 读完 socket 上的事件流

  private:
    int fd_ = -1;
    std::string ns_;
    std::thread th_;
    mutable std::mutex mu_;
    Status st_;
    RunResult res_;
    EventFn event_;
    std::atomic<bool> done_{false};
};

void RemoteRun::pump() {
    std::string buf;
    char tmp[8192];
    for (;;) {
        // 按行切事件
        for (std::size_t nl = buf.find('\n'); nl != std::string::npos; nl = buf.find('\n')) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            JValue v;
            if (json_parse(line, v) && v.t == JValue::T::Obj) {
                const JValue* ev = v.find("event");
                const std::string name = ev ? ev->as_string() : "";
                {
                    std::lock_guard lk(mu_);
                    if (name == "started") {
                        if (const JValue* x = v.find("ns")) ns_ = x->as_string();
                        if (const JValue* x = v.find("root")) st_.root = x->as_string();
                        if (const JValue* x = v.find("http_url")) st_.http_url = x->as_string();
                        if (const JValue* x = v.find("token")) st_.token = x->as_string();
                        st_.ns = ns_;
                        if (const JValue* x = v.find("script")) st_.script = x->as_string();
                    } else if (name == "log") {
                        if (const JValue* x = v.find("line")) {
                            if (st_.log.size() >= 2000) st_.log.erase(st_.log.begin());
                            st_.log.push_back(x->as_string());
                        }
                    } else if (name == "op") {
                        st_.op.name = v.find("op") ? v.find("op")->as_string() : "";
                        st_.op.detail = v.find("detail") ? v.find("detail")->as_string() : "";
                    } else if (name == "done") {
                        if (const JValue* x = v.find("ok")) res_.ok = x->as_bool(false);
                        if (const JValue* x = v.find("error")) res_.error = x->as_string();
                        if (const JValue* x = v.find("root")) res_.root = x->as_string();
                        if (const JValue* x = v.find("http_url")) res_.http_url = x->as_string();
                        if (const JValue* x = v.find("landlock")) res_.landlock = x->as_string();
                        if (const JValue* x = v.find("fs_ops")) res_.fs_ops = static_cast<std::uint64_t>(x->num);
                        if (const JValue* x = v.find("proc_runs"))
                            res_.proc_runs = static_cast<std::uint64_t>(x->num);
                        if (const JValue* x = v.find("net_requests"))
                            res_.net_requests = static_cast<std::uint64_t>(x->num);
                        res_.ns = ns_;
                        res_.token = st_.token;
                        res_.log = st_.log;
                        st_.run_state = res_.ok ? "done" : "failed";
                        st_.error = res_.error;
                    }
                }
                if (event_) {
                    std::lock_guard lk(mu_);
                    event_(line);
                }
                if (name == "done") {
                    done_ = true;
                    return;
                }
            }
        }
        pollfd p{fd_, POLLIN, 0};
        const int rc = ::poll(&p, 1, 1000);
        if (rc < 0) break;
        if (rc == 0) continue;  // 继续等（守护进程可能正跑一个长安装）
        const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
        if (buf.size() > 64 * 1024 * 1024) break;
    }
    ::close(fd_);
    fd_ = -1;
    if (!done_) {
        std::lock_guard lk(mu_);
        st_.run_state = "failed";
        st_.error = "the connection to the script service ended unexpectedly";
        res_.ok = false;
        res_.error = st_.error;
        res_.ns = ns_;
    }
    done_ = true;
}

}  // namespace

RunPtr daemon_submit(std::string_view socket_path, std::string script, std::string text,
                     std::string instance_dir, const Options& opt, const EventFn& event) {
    const std::string path = socket_path.empty() ? default_socket_path() : std::string(socket_path);
    const int fd = connect_unix(path);
    if (fd < 0) return nullptr;
    const std::string req = "{\"cmd\":\"run\",\"script\":" + json_quote(script) + ",\"text\":" +
                            json_quote(text) + ",\"instance\":" + json_quote(instance_dir) +
                            ",\"options\":" + options_to_json(opt) + "}\n";
    if (!write_all(fd, req)) {
        ::close(fd);
        return nullptr;
    }
    return std::make_shared<RemoteRun>(fd, opt.ns, event);
}

RunPtr ensure_daemon_submit(std::string_view socket_path, std::string script, std::string text,
                            std::string instance_dir, const Options& opt, const EventFn& event,
                            std::uint16_t preferred_port) {
    const std::string path = socket_path.empty() ? default_socket_path() : std::string(socket_path);
    if (auto run = daemon_submit(path, script, text, instance_dir, opt, event)) return run;
    // 拉一个守护进程：exec 自己，stdout/stderr 进 serve.log（避免占用调用方管道）
    const std::string log = socket_dir() + "/serve.log";
    const std::string port_arg = std::to_string(preferred_port ? preferred_port : kDefaultPort);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }
        const int out = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (out >= 0) {
            ::dup2(out, STDOUT_FILENO);
            ::dup2(out, STDERR_FILENO);
            ::close(out);
        }
        ::execl("/proc/self/exe", "mo-linux", "serve", "--detach", "--port", port_arg.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    if (pid < 0) return nullptr;
    int st = 0;
    ::waitpid(pid, &st, 0);  // serve --detach 自己会 double-fork 后立刻退出
    for (int i = 0; i < 100; ++i) {  // 等 socket 就绪（最多 ~10s）
        std::this_thread::sleep_for(milliseconds(100));
        if (auto run = daemon_submit(path, script, text, instance_dir, opt, event)) return run;
    }
    return nullptr;
}

}  // namespace mol::script
