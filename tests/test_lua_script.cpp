// Lua 安装脚本运行时的离线测试（tests/test_lua_script.cpp）。
// 只测 core/include/mol/lua_script.hpp 的公共契约：纯函数 + 本地 run（沙箱/vroot/预算/state）
// + HTTP server（token、namespace、_mol 自省）。全部在 /tmp 下的临时目录里跑，不联网、不跑 wine。
// 运行期行为以 core/src/lua_script.cpp 的当前实现为准；安全模型见 docs/DESIGN-lua-scripts.md。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "minitest.hpp"
#include "mol/lua_script.hpp"
#include "mol/mod_install.hpp"

namespace fs = std::filesystem;
using namespace mol;
namespace script = mol::script;

namespace {

using namespace std::chrono;

std::string tmp_dir(const std::string& tag) {
    const auto p = fs::temp_directory_path() / ("mol-script-test-" + tag + "-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p.string();
}

// 跑一个脚本：返回结果（阻塞到结束）。vroot 由调用方给定；instance 空 = 无实例。
script::RunResult run_script(const std::string& text, const std::string& root, script::Options opt = {},
                             const std::string& instance = {}) {
    opt.root = root;  // 虚拟根必须显式传给 local_run（否则会落到 <实例>/scripts/<名>.work）
    auto run = script::local_run("/tmp/test.lua", text, instance, std::move(opt), "script_0", {});
    run->start();
    run->join();
    return run->result();
}

// 最小 HTTP 客户端（测试用）：发一个请求，返回完整响应文本。
std::string http_request(std::uint16_t port, const std::string& head, const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return {};
    }
    const std::string req = head + body;
    std::size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
    std::string out;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

std::string http_get(std::uint16_t port, const std::string& path, const std::string& token) {
    return http_request(port, "GET " + path + (path.find('?') == std::string::npos ? "?" : "&") +
                                  "token=" + token + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                        "");
}

std::string http_post(std::uint16_t port, const std::string& path, const std::string& token,
                      const std::string& body) {
    return http_request(port, "POST " + path + "?token=" + token +
                                  " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n",
                        body);
}

std::string body_of(const std::string& response) {
    const auto p = response.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : response.substr(p + 4);
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------
TEST(vpath_grammar) {
    CHECK(script::valid_vpath("a"));
    CHECK(script::valid_vpath("a/b/c.txt"));
    CHECK(script::valid_vpath(".mol-logs/proc-1.log"));
    CHECK(script::valid_vpath("Data/meshes/x.nif"));
    CHECK(!script::valid_vpath(""));
    CHECK(!script::valid_vpath("/abs"));
    CHECK(!script::valid_vpath("a/../b"));
    CHECK(!script::valid_vpath(".."));
    CHECK(!script::valid_vpath("a//b"));
    CHECK(!script::valid_vpath("a/"));
    CHECK(!script::valid_vpath("a\\b"));
    CHECK(!script::valid_vpath("C:/x"));
    CHECK(!script::valid_vpath("a/\0b"));  // NUL 由长度/字符检查挡住
    CHECK(!script::valid_vpath(std::string(5000, 'a')));
    CHECK(!script::valid_vpath(std::string(300, 'a')));  // 组件 >255
    CHECK(!script::valid_vpath("a/./b"));
}

TEST(ns_and_state_key_grammar) {
    CHECK(script::valid_ns("script_0"));
    CHECK(script::valid_ns("skyrim-main"));
    CHECK(script::valid_ns("a1"));
    CHECK(!script::valid_ns("_mol"));     // 宿主保留
    CHECK(!script::valid_ns("Script_0"));  // 大写不行
    CHECK(!script::valid_ns(""));
    CHECK(!script::valid_ns("-x"));
    CHECK(!script::valid_ns(std::string(40, 'a')));
    CHECK(script::valid_state_key("phase"));
    CHECK(script::valid_state_key("xxx/xxx"));
    CHECK(script::valid_state_key("a.b-c_d"));
    CHECK(!script::valid_state_key("/x"));
    CHECK(!script::valid_state_key("a/../b"));
    CHECK(!script::valid_state_key("A"));
    CHECK(!script::valid_state_key(""));
}

TEST(url_scheme_only_http_https) {
    CHECK(script::http_url_ok("http://example.com/a"));
    CHECK(script::http_url_ok("https://cdn.nexusmods.com/x.7z?a=1"));
    CHECK(script::http_url_ok("HTTP://EXAMPLE.COM"));
    CHECK(!script::http_url_ok("file:///etc/passwd"));
    CHECK(!script::http_url_ok("ftp://example.com/x"));
    CHECK(!script::http_url_ok("gopher://example.com"));
    CHECK(!script::http_url_ok("http://"));
    CHECK(!script::http_url_ok("http://user@example.com/x"));  // userinfo
    CHECK(!script::http_url_ok("http://example.com/a b"));
    CHECK(!script::http_url_ok("example.com/a"));
}

TEST(private_ranges) {
    CHECK(script::host_is_private("127.0.0.1"));
    CHECK(script::host_is_private("10.1.2.3"));
    CHECK(script::host_is_private("192.168.1.1"));
    CHECK(script::host_is_private("172.16.0.1"));
    CHECK(script::host_is_private("169.254.169.254"));  // 云元数据
    CHECK(script::host_is_private("100.64.0.1"));
    CHECK(script::host_is_private("::1"));
    CHECK(script::host_is_private("fe80::1"));
    CHECK(script::host_is_private("fc00::1"));
    CHECK(script::host_is_private("::ffff:127.0.0.1"));
    CHECK(script::host_is_private("localhost"));  // 解析成回环
    CHECK(!script::host_is_private("93.184.216.34"));
    CHECK(!script::host_is_private("2606:2800:220:1:248:1893:25c8:1946"));
}

TEST(pe_machine_detection) {
    const std::string dir = tmp_dir("pe");
    const auto write = [&](const std::string& name, const std::string& bytes) {
        std::ofstream(fs::path(dir) / name, std::ios::binary) << bytes;
        return (fs::path(dir) / name).string();
    };
    auto pe = [](std::uint16_t machine) {
        std::string b(128, '\0');
        b[0] = 'M';
        b[1] = 'Z';
        std::uint32_t off = 0x40;
        std::memcpy(&b[0x3c], &off, 4);
        b[off] = 'P';
        b[off + 1] = 'E';
        std::memcpy(&b[off + 4], &machine, 2);
        return b;
    };
    CHECK_EQ(script::pe_machine(write("x86.exe", pe(0x14c))), std::string("i386"));
    CHECK_EQ(script::pe_machine(write("x64.exe", pe(0x8664))), std::string("amd64"));
    CHECK_EQ(script::pe_machine(write("arm.exe", pe(0xaa64))), std::string("arm64"));
    CHECK_EQ(script::pe_machine(write("bad.exe", std::string("not a pe at all"))), std::string("unknown"));
    CHECK_EQ(script::pe_machine(write("elf.bin", "\x7f" "ELFabc")), std::string("unknown"));
    CHECK_EQ(script::pe_machine((fs::path(dir) / "missing.exe").string()), std::string("unknown"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(unique_namespaces) {
    CHECK_EQ(script::unique_ns({}, ""), std::string("script_0"));
    CHECK_EQ(script::unique_ns({"script_0"}, ""), std::string("script_1"));
    CHECK_EQ(script::unique_ns({}, "skyrim-main"), std::string("skyrim-main"));
    CHECK_EQ(script::unique_ns({"skyrim-main"}, "skyrim-main"), std::string("skyrim-main-2"));
    bool threw = false;
    try {
        (void)script::unique_ns({}, "_mol");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// 虚拟文件系统
// ---------------------------------------------------------------------------
TEST(fs_roundtrip_inside_vroot) {
    const std::string root = tmp_dir("fs");
    const std::string text = R"(
        fs.mkdir('stage/Data')
        fs.write('stage/Data/a.txt', 'hello')
        fs.append('stage/Data/a.txt', ' world')
        assert(fs.read('stage/Data/a.txt') == 'hello world')
        assert(fs.exists('stage/Data/a.txt'))
        assert(fs.size('stage/Data/a.txt') == 11)
        fs.write('stage/Data/b.bin', string.char(0, 1, 2, 255))
        assert(#fs.read('stage/Data/b.bin') == 4)
        assert(fs.list('stage/Data')[1] == 'a.txt')
        assert(fs.list('stage/Data')[2] == 'b.bin')
        assert(fs.list('stage/nope') == nil)
        assert(fs.read('stage/Data/missing.txt') == nil)
        fs.copy('stage/Data/a.txt', 'stage/Data/c.txt')
        assert(fs.read('stage/Data/c.txt') == 'hello world')
        fs.move('stage/Data/c.txt', 'stage/Data/d.txt')
        assert(fs.exists('stage/Data/d.txt'))
        assert(not fs.exists('stage/Data/c.txt'))
        fs.remove('stage/Data/d.txt')
        assert(not fs.exists('stage/Data/d.txt'))
        fs.remove('stage')
        assert(not fs.exists('stage'))
        log.info('fs ok')
    )";
    const auto r = run_script(text, root);
    CHECK(r.ok);
    CHECK_EQ(std::string(r.error), std::string(""));
    std::error_code ec;
    CHECK(!fs::exists(fs::path(root) / "stage", ec));
    fs::remove_all(root, ec);
}

// 逐个坏路径各跑一次：脚本里没有 pcall，第一次宿主调用失败整个脚本就失败
TEST(fs_rejects_escapes_and_symlinks) {
    const std::string root = tmp_dir("escape");
    std::error_code ec;
    fs::create_directories(fs::path(root) / "in", ec);
    fs::create_directories("/tmp/mol-script-test-outside", ec);
    fs::create_symlink("/tmp/mol-script-test-outside", fs::path(root) / "in" / "link", ec);
    struct Case {
        const char* what;
        std::string code;
        const char* expect;
    };
    const Case cases[] = {
        {"absolute path", "fs.read('/etc/hostname')", "invalid virtual path"},
        {"traversal", "fs.read('../../etc/hostname')", "invalid virtual path"},
        {"dot component", "fs.read('a/./b')", "invalid virtual path"},
        {"backslash", "fs.read('a\\b')", "invalid virtual path"},
        {"drive letter", "fs.read('C:/x')", "invalid virtual path"},
        {"read through symlink", "fs.read('in/link/x')", "cannot"},
        {"list through symlink", "fs.list('in/link')", "cannot"},
        {"write through symlink", "fs.write('in/link/x', 'y')", "cannot"},

        {"copy through symlink", "fs.copy('in/link/x', 'z')", "cannot"},
        {"move through symlink", "fs.move('in/link/x', 'z')", "cannot"},
        {"write outside", "fs.write('../escape.txt', 'x')", "invalid virtual path"},
        {"missing parent", "fs.write('nodir/x.txt', 'y')", "cannot"},
    };
    for (const auto& c : cases) {
        const auto r = run_script(c.code, root);
        CHECK(!r.ok);
        if (!r.ok) CHECK(contains(r.error, c.expect));
    }
    // 事后确认：外部目录没被写进去
    CHECK(fs::is_empty("/tmp/mol-script-test-outside", ec));
    CHECK(!fs::exists("/tmp/mol-script-test-outside/escape.txt", ec));
    fs::remove_all(root, ec);
    fs::remove_all("/tmp/mol-script-test-outside", ec);
}

TEST(symlinked_vroot_itself_is_not_followed_out) {
    // vroot 里出现指向外部的链接时，宿主侧（net/archive 的事后校验）也不该被绕过：
    // 这里验证脚本无法通过 fs 读到外部文件内容
    const std::string root = tmp_dir("sym");
    std::error_code ec;
    fs::create_directories("/tmp/mol-script-test-secret", ec);
    {
        std::ofstream("/tmp/mol-script-test-secret/passwd") << "TOPSECRET";
    }
    fs::create_symlink("/tmp/mol-script-test-secret/passwd", fs::path(root) / "leak", ec);
    const auto r = run_script("local x = fs.read('leak')\nassert(false, 'read ' .. tostring(x))", root);
    CHECK(!r.ok);
    fs::remove_all(root, ec);
    fs::remove_all("/tmp/mol-script-test-secret", ec);
}

TEST(fs_write_limits) {
    const std::string root = tmp_dir("limits");
    script::Options opt;
    opt.limits.file_bytes = 16;   // 单文件 16 字节
    opt.limits.vroot_bytes = 64;  // vroot 总共 64 字节
    auto r = run_script("fs.write('big.txt', string.rep('x', 100))", root, opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "per-file"));
    r = run_script("for i = 1, 10 do fs.write('f' .. i .. '.txt', string.rep('x', 10)) end", root, opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "budget"));
    // 父目录不存在 → 明确失败（不会偷偷建目录）
    r = run_script("fs.write('nodir/x.txt', 'y')", root);
    CHECK(!r.ok);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(fs_op_count_limit) {
    const std::string root = tmp_dir("ops");
    script::Options opt;
    opt.limits.fs_ops = 50;
    const auto r = run_script("for i = 1, 1000 do fs.exists('x') end", root, opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "filesystem operations"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// 预算：死循环 / 内存炸弹 / 墙钟
// ---------------------------------------------------------------------------
TEST(infinite_loop_is_bounded) {
    const std::string root = tmp_dir("loop");
    script::Options opt;
    opt.limits.instructions = 20'000'000;  // 2000 万指令 ≈ 毫秒级
    const auto r = run_script("local i = 0\nwhile true do i = i + 1 end", root, opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "instruction budget"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(wall_clock_deadline) {
    const std::string root = tmp_dir("wall");
    script::Options opt;
    opt.limits.wall_ms = 300;
    const auto r = run_script("local i = 0\nwhile true do i = i + 1 end", root, opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "wall clock"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(memory_bomb_is_bounded) {
    const std::string root = tmp_dir("mem");
    script::Options opt;
    opt.limits.memory_bytes = 2 * 1024 * 1024;  // 2 MiB
    const auto r = run_script("local t = {}\nfor i = 1, 100000000 do t[i] = string.rep('x', 64) end", root, opt);
    CHECK(!r.ok);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// state / log / 自省
// ---------------------------------------------------------------------------
TEST(state_roundtrip) {
    const std::string root = tmp_dir("state");
    const std::string text = R"(
        state.set('phase', 'staging')
        state.set('xxx/xxx', 42)
        state.set('flag', true)
        assert(state.get('phase') == 'staging')
        assert(state.get('xxx/xxx') == '42')
        assert(state.get('flag') == 'true')
        assert(state.get('nope') == nil)
        assert(state.get('nope', 'dflt') == 'dflt')
        local keys = state.keys()
        assert(#keys == 3)
        state.delete('flag')
        assert(state.get('flag') == nil)
        assert(#state.keys() == 2)
    )";
    const auto r = run_script(text, root);
    CHECK(r.ok);
    CHECK_EQ(r.state.size(), std::size_t{2});
    CHECK_EQ(r.state.at("phase"), std::string("staging"));
    CHECK_EQ(r.state.at("xxx/xxx"), std::string("42"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(state_rejects_bad_keys_and_types) {
    const std::string root = tmp_dir("statebad");
    auto r = run_script("state.set('/abs', 'x')", root);
    CHECK(!r.ok);
    r = run_script("state.set('a/../b', 'x')", root);
    CHECK(!r.ok);
    r = run_script("state.set('k', {})", root);
    CHECK(!r.ok);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(introspection_sees_stuck_loop) {
    // 卡在循环里：行号不动、指令数在涨——这正是 _mol 自省要能看出的东西
    const std::string root = tmp_dir("stuck");
    script::Options opt;
    opt.limits.wall_ms = 3000;
    auto run = script::local_run("/tmp/spin.lua", "local i = 0\nwhile true do i = i + 1 end", {}, std::move(opt),
                                 "script_0", {});
    run->start();
    int line1 = -1, line2 = -1;
    std::uint64_t instr1 = 0, instr2 = 0;
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(milliseconds(25));
        const auto st = run->status();
        if (line1 < 0 && st.lua_line > 0) {
            line1 = st.lua_line;
            instr1 = st.instructions;
        } else if (line1 > 0) {
            line2 = st.lua_line;
            instr2 = st.instructions;
            break;
        }
    }
    run->join();
    CHECK(line1 > 0);
    CHECK_EQ(line1, line2);       // 死循环：停在同一行
    CHECK(instr2 > instr1);       // 指令数在涨
    CHECK(!run->result().ok);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(dry_run_disables_proc_and_net) {
    const std::string root = tmp_dir("dry");
    script::Options opt;
    opt.dry_run = true;
    auto r = run_script("net.get('http://example.com')", root, opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "dry-run"));
    // 真 PE 也会被 dry-run 挡下（校验先过，再撞 dry-run 开关）
    const std::string pe_lua =
        "string.char(0x4d,0x5a) .. string.rep('\\0',0x3a) .. string.char(0x40,0,0,0)"
        " .. 'PE' .. string.char(0,0) .. string.char(0x64,0x86) .. string.rep('\\0',0x100)";
    r = run_script("fs.write('setup.exe', " + pe_lua + ")\nproc.run('setup.exe')", root, opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "dry-run"));
    // fs 仍然可用，且 vroot 是临时目录（不碰实例）
    r = run_script("fs.write('a.txt', 'ok')\nassert(fs.read('a.txt') == 'ok')", root, opt);
    CHECK(r.ok);
    CHECK(contains(r.root, "mol-script-test-"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// proc.run 的校验（不起 wine）：只跑 .exe / PE / vroot 内
// ---------------------------------------------------------------------------
TEST(proc_run_refuses_non_pe_and_non_exe) {
    const std::string root = tmp_dir("proc2");
    const std::string prep = "fs.write('notepad.exe', 'this is not a PE file')\n";
    auto r = run_script(prep + "proc.run('notepad.exe')", root);
    CHECK(!r.ok);
    CHECK(contains(r.error, "PE"));
    r = run_script("fs.write('tool.bin', 'x')\nproc.run('tool.bin')", root);
    CHECK(!r.ok);
    CHECK(contains(r.error, ".exe"));
    r = run_script("proc.run('missing.exe')", root);
    CHECK(!r.ok);
    CHECK(contains(r.error, "not found"));
    r = run_script("proc.run('/bin/sh')", root);
    CHECK(!r.ok);
    CHECK(contains(r.error, "virtual path") || contains(r.error, ".exe"));
    r = run_script("proc.run('../x.exe')", root);
    CHECK(!r.ok);
    CHECK(contains(r.error, "virtual path"));
    // 没有实例时连 runner 都没有
    r = run_script("fs.write('a.exe', 'MZ' .. string.rep('x', 200))\nproc.run('a.exe')", root);
    CHECK(!r.ok);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// HTTP server：token / namespace / _mol 自省
// ---------------------------------------------------------------------------
TEST(http_namespace_and_introspection) {
    const std::string root = tmp_dir("http");
    auto reg = script::make_registry();
    script::HttpServer server(*reg, 0);  // 临时端口
    const auto port = server.port();
    CHECK(port != 0);
    const std::string token = server.token();
    CHECK_EQ(token.size(), std::size_t{32});

    // 没有 token → 401
    const auto denied = http_get(port, "/script_0/state", "wrong-token");
    CHECK(contains(denied, "401"));
    // 未知 namespace → 404
    CHECK(contains(http_get(port, "/nope/state", token), "404"));

    // 一个长期运行的脚本（自旋），另一个 run 占着 script_0
    script::Options opt;
    opt.limits.wall_ms = 2500;
    auto run = script::local_run("/tmp/serve.lua", "state.set('phase', 'running')\nlocal i = 0\nwhile true do i = i + 1 end",
                                 {}, std::move(opt), "script_0", {});
    run->start();
    reg->add(run);
    std::this_thread::sleep_for(milliseconds(300));

    // 宿主自省：ping / 列表 / 单个 run
    const auto ping = body_of(http_get(port, "/_mol/ping", token));
    CHECK(contains(ping, "\"ok\":true"));
    const auto list = body_of(http_get(port, "/_mol/scripts", token));
    CHECK(contains(list, "script_0"));
    CHECK(contains(list, "\"op\":\"\"") || contains(list, "lua_line"));  // 在跑：行号在报
    const auto one = body_of(http_get(port, "/_mol/scripts/script_0", token));
    CHECK(contains(one, "\"run_state\":\"running\""));
    CHECK(contains(one, "\"lua_line\":") && !contains(one, "\"lua_line\":0"));

    // 脚本 namespace：读/改 state（用户可以在浏览器里看和改）
    const auto st = body_of(http_get(port, "/script_0/state", token));
    CHECK(contains(st, "\"phase\":\"running\""));
    const auto posted = http_post(port, "/script_0/state/note", token, "hello from the browser");
    CHECK(contains(posted, "200"));
    const auto note = body_of(http_get(port, "/script_0/state/note", token));
    CHECK(contains(note, "hello from the browser"));
    // 用户例子里的路径型 key
    CHECK(contains(http_post(port, "/script_0/xxx/xxx", token, "deep"), "200"));
    CHECK(contains(body_of(http_get(port, "/script_0/xxx/xxx", token)), "deep"));
    // 非法 key → 400
    CHECK(contains(http_post(port, "/script_0/../etc", token, "x"), "400"));

    run->join();
    std::this_thread::sleep_for(milliseconds(100));
    const auto after = body_of(http_get(port, "/_mol/scripts/script_0", token));
    CHECK(contains(after, "\"run_state\":\"failed\""));
    reg->remove("script_0");
    CHECK(contains(http_get(port, "/script_0/state", token), "404"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(registry_rejects_duplicate_namespace) {
    auto reg = script::make_registry();
    auto run = script::local_run("/tmp/a.lua", "log.info('x')", {}, {}, "script_0", {});
    reg->add(run);
    auto run2 = script::local_run("/tmp/b.lua", "log.info('y')", {}, {}, "script_0", {});
    bool threw = false;
    try {
        reg->add(run2);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(reg->size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// WP2：实例写 API（创建实例 / 装 mod / 部署）
// ---------------------------------------------------------------------------
namespace {

// 建一个最小实例（游戏目录 + instance init），返回实例根
std::string make_instance(const std::string& tag) {
    const std::string base = tmp_dir(tag);
    const std::string game = base + "/game";
    const std::string root = base + "/instance";
    const std::string prefix = base + "/instance/prefix";
    std::error_code ec;
    fs::create_directories(game, ec);
    std::ofstream(fs::path(game) / "SkyrimSE.exe", std::ios::binary) << "fixture";
    mol::InitOptions io;  // 持 string_view：局部 string 必须活过这次调用
    io.root = root;
    io.game_dir = game;
    io.prefix = prefix;
    io.runner_kind = "wine";
    io.profile = "Default";
    try {
        mol::init_instance(io);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  make_instance failed: %s\n", e.what());
        throw;
    }
    return root;
}

void put_file(const std::string& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream(fs::path(path), std::ios::binary) << body;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool have_7z() { return std::system("command -v 7z >/dev/null 2>&1") == 0; }

}  // namespace

// 装 stage 好的目录 → modlist → 部署到农场；并验证 enable/disable/list/info
TEST(wp2_install_staged_enable_and_apply) {
    const std::string inst_root = make_instance("wp2a");
    const std::string root = tmp_dir("wp2a-root");
    const std::string text = R"(
        local info = instance.info()
        assert(info.root == '@ROOT@', info.root)
        assert(info.game == 'skyrimse')
        assert(info.runner == 'wine')
        fs.mkdir('stage/Data/meshes')
        fs.write('stage/Data/meshes/a.nif', 'nif-bytes')
        local m = mods.install_staged('Staged Mod', 'stage')
        assert(m.name == 'Staged Mod')
        assert(m.files == 1)
        assert(m.root == false)
        assert(mods.enable('Staged Mod').enabled == true)
        local listed = 0
        for _, row in ipairs(mods.list()) do
            if row.name == 'Staged Mod' then listed = listed + 1; assert(row.enabled) end
        end
        assert(listed == 1)
        local f = farm.apply()
        assert(f.applied >= 1)
    )";
    std::string script_text = text;
    const auto pos = script_text.find("@ROOT@");
    script_text.replace(pos, 6, inst_root);
    const auto r = run_script(script_text, root, {}, inst_root);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "  wp2 error: %s\n", r.error.c_str());
    // 磁盘结果：mod 目录、modlist、农场
    CHECK(fs::exists(fs::path(inst_root) / "mods/Staged Mod/meshes/a.nif"));
    CHECK(read_file((fs::path(inst_root) / "profiles/Default/modlist.txt").string()).find("+Staged Mod") !=
          std::string::npos);
    CHECK(fs::exists(fs::path(inst_root) / "farm/Data/meshes/a.nif"));
    // 原 stage 文件没被搬走（install_directory 是复制）
    CHECK(fs::exists(fs::path(root) / "stage/Data/meshes/a.nif"));
    std::error_code ec;
    fs::remove_all(inst_root, ec);
    fs::remove_all(root, ec);
}

// 脚本自己建实例：从零安装一个"游戏"
TEST(wp2_instance_create_from_nothing) {
    const std::string base = tmp_dir("wp2b");
    const std::string game = base + "/game";
    std::error_code ec;
    fs::create_directories(game, ec);
    put_file(game + "/SkyrimSE.exe", "fixture");
    const std::string new_root = base + "/instance";
    const std::string text = R"(
        local created = instance.create{
            root = '@ROOT@',
            game_dir = '@GAME@',
            prefix = '@PREFIX@',
            runner = 'wine',
            profile = 'Default',
        }
        assert(created.root == '@ROOT@', created.root)
        assert(created.changed == true)
        local info = instance.info()
        assert(info.root == '@ROOT@')
        fs.mkdir('stage/Data')
        fs.write('stage/Data/readme.txt', 'from zero')
        local m = mods.install_staged('From Zero', 'stage')
        assert(m.name == 'From Zero')
        farm.apply()
        state.set('instance', created.root)
    )";
    std::string script_text = text;
    for (const auto& [ph, value] : std::vector<std::pair<const char*, std::string>>{
             {"@ROOT@", new_root}, {"@GAME@", game}, {"@PREFIX@", new_root + "/prefix"}}) {
        for (std::size_t p; (p = script_text.find(ph)) != std::string::npos;)
            script_text.replace(p, 6, value);
    }
    // 没有实例的 run：instance_dir 传空
    auto run = script::local_run("/tmp/fromzero.lua", script_text, {}, {}, "script_0", {});
    run->start();
    run->join();
    const auto r = run->result();
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "  wp2 create error: %s\n", r.error.c_str());
    CHECK(fs::exists(fs::path(new_root) / "mo-linux.json"));
    CHECK(fs::exists(fs::path(new_root) / "mods/From Zero/readme.txt"));
    CHECK(fs::exists(fs::path(new_root) / "farm/Data/readme.txt"));
    fs::remove_all(base, ec);
}

// dry-run 一律拒绝写实例
TEST(wp2_dry_run_blocks_instance_writes) {
    const std::string inst_root = make_instance("wp2c");
    script::Options opt;
    opt.dry_run = true;
    auto r = run_script("mods.enable('x')", tmp_dir("wp2c-root"), opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "dry-run"));
    r = run_script("instance.create{root='/tmp/x', game_dir='/tmp'}", tmp_dir("wp2c-root"), opt);
    CHECK(!r.ok);
    CHECK(contains(r.error, "dry-run"));
    // 没有实例时 mods.* 也要明确报错（而不是崩）
    r = run_script("mods.list()", tmp_dir("wp2c-root"));
    CHECK(!r.ok);
    CHECK(contains(r.error, "no instance"));
    std::error_code ec;
    fs::remove_all(inst_root, ec);
}

// 从虚拟根里的压缩包直接装 mod（没有 FOMOD 时按普通包）
TEST(wp2_install_archive_from_virtual_root) {
    if (!have_7z()) return;
    const std::string inst_root = make_instance("wp2d");
    const std::string root = tmp_dir("wp2d-root");
    put_file(root + "/pkg/Data/textures/t.dds", "dds");
    put_file(root + "/pkg/Data/readme.txt", "readme");
    const std::string zip = root + "/pkg.7z";
    CHECK_EQ(std::system(("cd '" + root + "/pkg' && 7z a -bd '" + zip + "' . >/dev/null 2>&1").c_str()), 0);
    const auto r = run_script(
        "local m = mods.install_archive('pkg.7z', 'Archive Mod')\n"
        "assert(m.name == 'Archive Mod')\n"
        "assert(m.files == 2)\n"
        "farm.apply()",
        root, {}, inst_root);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "  wp2 archive error: %s\n", r.error.c_str());
    CHECK(fs::exists(fs::path(inst_root) / "mods/Archive Mod/textures/t.dds"));
    CHECK(fs::exists(fs::path(inst_root) / "farm/Data/textures/t.dds"));
    std::error_code ec;
    fs::remove_all(inst_root, ec);
    fs::remove_all(root, ec);
}

// 逃逸的 stage 目录（含指向外部的符号链接）必须被拒，且不留下半个 mod
TEST(wp2_install_staged_refuses_symlink_escape) {
    const std::string inst_root = make_instance("wp2e");
    const std::string root = tmp_dir("wp2e-root");
    std::error_code ec;
    fs::create_directories("/tmp/mol-script-test-wp2-secret", ec);
    put_file("/tmp/mol-script-test-wp2-secret/passwd", "TOPSECRET");
    put_file(root + "/stage/Data/ok.txt", "fine");
    fs::create_symlink("/tmp/mol-script-test-wp2-secret/passwd", fs::path(root) / "stage/Data/leak", ec);
    const auto r = run_script("mods.install_staged('Evil', 'stage')", root, {}, inst_root);
    CHECK(!r.ok);
    CHECK(contains(r.error, "symbolic link"));
    CHECK(contains(r.error, "symbolic link"));
    CHECK(!fs::exists(fs::path(inst_root) / "mods/Evil"));
    fs::remove_all(inst_root, ec);
    fs::remove_all(root, ec);
    fs::remove_all("/tmp/mol-script-test-wp2-secret", ec);
}
