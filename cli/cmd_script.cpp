// script run / serve：Lua 安装脚本入口（安全模型见 docs/DESIGN-lua-scripts.md，API 见 docs/LUA-SCRIPTS.md）。
//
// script run 默认把脚本交给常驻 service（`mo-linux serve`，dockerd 形态：一个端口 + 全局 namespace
// 注册表），没有就拉一个；--local 在当前进程里跑（测试/一次性/不想留后台时用）。两种模式跑同一份沙箱。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include "mol/lua_script.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {
namespace {

// 读脚本源码（上限由 Limits 决定；超大直接拒绝，避免把大文件读进内存再拒绝）。
std::string read_script(Context& ctx, std::size_t cap) {
    std::ifstream f(std::string(ctx.args.positionals.at(0)), std::ios::binary);
    if (!f) throw mol::Error("invalid_argument", "cannot read the Lua script");
    std::string text(cap + 1, '\0');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(f.gcount()));
    if (text.size() > cap)
        throw mol::Error("invalid_argument",
                         "the script is larger than the " + std::to_string(cap / 1024) + " KiB source limit");
    return text;
}

std::uint16_t opt_port(Context& ctx) {
    const mol::string s = ctx.args.get("port", "", ctx.mem);
    if (s.empty()) return mol::script::kDefaultPort;
    const long v = std::strtol(s.c_str(), nullptr, 10);
    if (v < 1 || v > 65535) throw mol::Error("invalid_argument", "--port must be 1-65535");
    return static_cast<std::uint16_t>(v);
}

// 事件行里的 "line" 字段 → 明文（只解一层 JSON 转义，够日志用）
std::string event_line_text(const std::string& line) {
    const auto p = line.find("\"line\":\"");
    if (p == std::string::npos) return {};
    const std::size_t b = p + 8;
    const std::size_t e = line.find('"', b);
    std::string msg;
    for (std::size_t i = b; i < e && i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            ++i;
            switch (line[i]) {
                case 'n': msg.push_back('\n'); break;
                case 't': msg.push_back('\t'); break;
                default: msg.push_back(line[i]);
            }
        } else {
            msg.push_back(line[i]);
        }
    }
    return msg;
}

mol::script::EventFn log_sink(Context& ctx) {
    const bool quiet = ctx.globals && ctx.globals->quiet;
    return [quiet](const std::string& line) {
        if (quiet) return;
        if (line.find("\"event\":\"log\"") == std::string::npos) return;
        const std::string msg = event_line_text(line);
        if (!msg.empty()) std::fprintf(stderr, "script: %s\n", msg.c_str());
    };
}

}  // namespace

Result run_script_run(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    if (auto bad = check_positionals(ctx, 1, "LUA_FILE")) return *bad;
    const bool local = ctx.args.get_bool("--local", false);
    const bool dry = ctx.args.get_bool("--dry-run", false);
    const bool clean = ctx.args.get_bool("--clean", false);

    mol::script::Options opt;
    opt.limits.source_bytes = 256 * 1024;
    if (ctx.args.has("--timeout")) {
        const long m = std::strtol(ctx.args.get("--timeout", "", ctx.mem).c_str(), nullptr, 10);
        if (m < 1 || m > 1440) return make_usage_error("script run: --timeout must be 1-1440 minutes", ctx);
        opt.limits.wall_ms = static_cast<std::uint64_t>(m) * 60 * 1000;
    }
    if (ctx.args.has("--memory")) {
        const long mb = std::strtol(ctx.args.get("--memory", "", ctx.mem).c_str(), nullptr, 10);
        if (mb < 1 || mb > 128) return make_usage_error("script run: --memory must be 1-128 MiB", ctx);
        opt.limits.memory_bytes = static_cast<std::size_t>(mb) * 1024 * 1024;
    }
    opt.ns = std::string(ctx.args.get("--ns", "", ctx.mem));
    opt.root = std::string(ctx.args.get("--root", "", ctx.mem));
    opt.net = !ctx.args.get_bool("--no-net", false);
    opt.deny_private = ctx.args.get_bool("--deny-private", false);
    opt.dry_run = dry;
    if (!opt.ns.empty() && !mol::script::valid_ns(opt.ns))
        return make_usage_error("script run: --ns must match [a-z0-9][a-z0-9_-]{0,31} "
                                "(names starting with '_' are reserved for the host)",
                                ctx);

    const std::string script = std::string(ctx.args.positionals.at(0));
    const std::string text = read_script(ctx, opt.limits.source_bytes);
    if (clean && !opt.root.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(opt.root, ec);
    }

    // 实例：vroot 默认挂在实例下；proc.run / mods.* / farm.* 也要实例。
    // --dry-run 用临时 vroot，--no-instance 允许「脚本自己建实例」的从零安装流程。
    std::string instance_dir;
    try {
        const mol::Instance inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
        instance_dir = std::string(inst.root);
    } catch (const mol::Error&) {
        if (!dry && !ctx.args.get_bool("--no-instance", false))
            throw mol::Error("instance_not_found",
                             "no instance here; run `instance init` first, or pass --no-instance for a "
                             "script that creates its own instance (instance.create)");
        if (ctx.globals && !ctx.globals->quiet)
            std::fprintf(stderr, "script: no instance; the script must create one with instance.create\n");
    }

    auto say = [&ctx](const std::string& line) {
        if (ctx.globals && ctx.globals->quiet) return;
        std::fprintf(stderr, "%s\n", line.c_str());
    };

    mol::script::RunPtr run;
    std::string mode, token;
    std::unique_ptr<mol::script::HttpServer> server;  // 必须活到 run 结束（脚本的 HTTP namespace）
    if (local || dry) {
        auto reg = mol::script::make_registry();
        std::vector<std::string> taken;
        const std::string ns = mol::script::unique_ns(taken, opt.ns);
        server = std::make_unique<mol::script::HttpServer>(*reg, opt_port(ctx));
        token = server->token();
        opt.http_url = server->url_for(ns);
        opt.token = token;
        run = mol::script::local_run(script, text, instance_dir, opt, ns, log_sink(ctx));
        reg->add(run);
        run->start();
        const auto st = run->status();
        say("script: virtual root " + st.root);
        say("script: state at " + st.http_url + "?token=" + token);
        mode = "local";
    } else {
        run = mol::script::ensure_daemon_submit({}, script, text, instance_dir, opt, log_sink(ctx),
                                                opt_port(ctx));
        if (!run)
            throw mol::Error("io_error",
                             "cannot reach the script service and could not start one "
                             "(try `mo-linux serve --detach`, or `script run --local`)");
        mode = "service";
        // 等 started 事件回来（namespace / URL / token），再告诉用户去哪看
        for (int i = 0; i < 50; ++i) {
            const auto st = run->status();
            if (!st.http_url.empty() || run->done()) {
                if (!st.http_url.empty())
                    say("script: namespace " + st.ns + ", state at " + st.http_url + "?token=" + st.token);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    run->join();
    const mol::script::RunResult res = run->result();

    ScriptRunData d{.script = mol::string(script, ctx.mem),
                    .ns = mol::string(res.ns, ctx.mem),
                    .mode = mol::string(mode, ctx.mem),
                    .ok = res.ok,
                    .error = mol::string(res.error, ctx.mem),
                    .root = mol::string(res.root, ctx.mem),
                    .http_url = mol::string(res.http_url, ctx.mem),
                    .token = mol::string(res.token, ctx.mem),
                    .landlock = mol::string(res.landlock, ctx.mem),
                    .fs_ops = res.fs_ops,
                    .proc_runs = res.proc_runs,
                    .net_requests = res.net_requests,
                    .log = std::pmr::vector<std::pmr::string>(ctx.mem),
                    .state = std::pmr::vector<ScriptStateRow>(ctx.mem)};
    for (const auto& l : res.log) d.log.push_back(std::pmr::string(l, ctx.mem));
    for (const auto& [k, v] : res.state)
        d.state.push_back(ScriptStateRow{std::pmr::string(k, ctx.mem), std::pmr::string(v, ctx.mem)});
    if (!res.ok && res.error.empty()) d.error = mol::string("the script failed", ctx.mem);
    Result r(ctx.mem);
    r.ok = res.ok;
    r.exit_code = res.ok ? 0 : 1;
    r.command = ctx.command;
    if (!res.ok) r.add_error("script_failed", d.error, d.root);
    if (res.landlock == "unavailable")
        r.add_warning("landlock_unavailable",
                      "this kernel has no Landlock, so executables the script ran were not contained "
                      "(kernel 5.13+ is required)",
                      "");
    r.set_data(std::move(d));
    return r;
}

Result run_serve(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const mol::string socket_arg = ctx.args.get("--socket", "", ctx.mem);
    const std::string socket_path =
        socket_arg.empty() ? mol::script::default_socket_path() : std::string(socket_arg);
    if (ctx.args.get_bool("--stop", false)) {
        // 直连控制 socket 发 shutdown（协议见 core/src/lua_script_serve.cpp）
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        bool connected = false;
        if (fd >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, socket_path.c_str(), sizeof addr.sun_path - 1);
            connected = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
            if (!connected) ::close(fd);
        }
        if (!connected)
            throw mol::Error("io_error", "no script service is listening on " + socket_path, socket_path);
        const std::string req = "{\"cmd\":\"shutdown\"}\n";
        std::size_t sent = 0;
        while (sent < req.size()) {
            const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
        ::shutdown(fd, SHUT_WR);
        char buf[512];
        while (::recv(fd, buf, sizeof buf, 0) > 0) {
        }
        ::close(fd);
        ServeData d{.socket = mol::string(socket_path, ctx.mem), .http = mol::string("", ctx.mem), .port = 0,
                    .detached = false};
        Result r(ctx.mem);
        r.command = ctx.command;
        r.set_data(std::move(d));
        return r;
    }
    std::uint16_t port = mol::script::kDefaultPort;
    if (ctx.args.has("--port")) {
        const long v = std::strtol(ctx.args.get("--port", "", ctx.mem).c_str(), nullptr, 10);
        if (v < 1 || v > 65535) return make_usage_error("serve: --port must be 1-65535", ctx);
        port = static_cast<std::uint16_t>(v);
    }
    int idle = 600;
    if (ctx.args.has("--idle-timeout")) {
        idle = static_cast<int>(std::strtol(ctx.args.get("--idle-timeout", "", ctx.mem).c_str(), nullptr, 10));
        if (idle < 0 || idle > 86400) return make_usage_error("serve: --idle-timeout must be 0-86400", ctx);
    }

    if (ctx.args.get_bool("--detach", false)) {
        // double-fork：孙进程 exec 自己跑前台 serve，CLI 等它就绪后返回
        const std::string port_arg = std::to_string(port);
        const std::string idle_arg = std::to_string(idle);
        const std::string log = socket_path.substr(0, socket_path.find_last_of('/')) + "/serve.log";
        const pid_t pid = ::fork();
        if (pid < 0) throw mol::Error("io_error", "fork failed");
        if (pid == 0) {
            ::setsid();
            const pid_t second = ::fork();
            if (second > 0) ::_exit(0);
            if (second < 0) ::_exit(1);
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
            ::execl("/proc/self/exe", "mo-linux", "serve", "--port", port_arg.c_str(), "--idle-timeout",
                    idle_arg.c_str(), "--socket", socket_path.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        for (int i = 0; i < 100; ++i) {  // 等 service 就绪（最多 ~10s）
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::uint16_t actual = 0;
            if (mol::script::daemon_ping(socket_path, &actual)) {
                ServeData d{.socket = mol::string(socket_path, ctx.mem),
                            .http = mol::string("http://127.0.0.1:" + std::to_string(actual), ctx.mem),
                            .port = actual,
                            .detached = true};
                Result r(ctx.mem);
                r.command = ctx.command;
                r.set_data(std::move(d));
                return r;
            }
        }
        throw mol::Error("io_error", "the script service did not come up (see " + log + ")", log);
    }

    std::uint16_t actual = 0;
    const int code = mol::script::serve_main(port, idle, socket_path, &actual);
    if (code != 0)
        throw mol::Error("io_error", "the script service exited with code " + std::to_string(code));
    ServeData d{.socket = mol::string(socket_path, ctx.mem),
                .http = mol::string("http://127.0.0.1:" + std::to_string(actual), ctx.mem),
                .port = actual,
                .detached = false};
    Result r(ctx.mem);
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
