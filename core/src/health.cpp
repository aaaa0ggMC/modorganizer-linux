#include "mol/health.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <thread>

#include <unistd.h>

#include "mol/casefold.hpp"
#include "mol/mo2fmt.hpp"
#include "mol/mod_install.hpp"
#include "mol/runner.hpp"

namespace mol::health {
namespace fs = std::filesystem;
namespace {

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path prefix_user_dir(const Instance& inst) {
    return fs::path(std::string(inst.cfg.prefix)) / "drive_c/users" / std::string(inst.cfg.prefix_user.empty() ? std::string_view("steamuser") : std::string_view(inst.cfg.prefix_user));
}

// dir 下大小写不敏感地找名为 name 的条目；找不到返回空
fs::path child_ci(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    const auto want = casefold(name);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (casefold(it->path().filename().string()) == want) return it->path();
    return {};
}
// 逐级大小写不敏感地解析 "a/b/c"
fs::path resolve_ci(fs::path cur, std::string_view rel) {
    std::size_t i = 0;
    while (i <= rel.size()) {
        std::size_t j = rel.find('/', i);
        if (j == std::string_view::npos) j = rel.size();
        if (j > i) {
            cur = child_ci(cur, rel.substr(i, j - i));
            if (cur.empty()) return {};
        }
        i = j + 1;
    }
    return cur;
}

bool all_digits(std::string_view s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }

std::uint32_t le32(const std::string& b, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16 | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24;
}

// SKSE 日志一行里的插件名：「plugin <名字> (…」或「couldn't load plugin <路径> (…」→ 文件名部分
std::string plugin_in_line(std::string_view line) {
    auto p = line.find("plugin ");
    if (p == std::string_view::npos) return std::string(line);
    std::string_view rest = line.substr(p + 7);
    const auto paren = rest.find(" (");
    if (paren != std::string_view::npos) rest = rest.substr(0, paren);
    const auto slash = rest.find_last_of("\\/");
    if (slash != std::string_view::npos) rest = rest.substr(slash + 1);
    while (!rest.empty() && (rest.back() == ':' || rest.back() == ' ')) rest.remove_suffix(1);
    return std::string(rest);
}

}  // namespace

fs::path appdata_dir(const Instance& inst) { return prefix_user_dir(inst) / "AppData/Local/Skyrim Special Edition"; }
fs::path my_games_dir(const Instance& inst) { return prefix_user_dir(inst) / "Documents/My Games/Skyrim Special Edition"; }

// ---- D1 ----------------------------------------------------------------------------------
std::vector<std::string> bad_catalog_versions(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while ((i = text.find("\"Version\"", i)) != std::string_view::npos) {
        i += 9;
        std::size_t j = i;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) ++j;
        if (j >= text.size() || text[j] != ':') continue;
        ++j;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) ++j;
        if (j >= text.size() || text[j] != '"') continue;
        const auto end = text.find('"', j + 1);
        if (end == std::string_view::npos) break;
        const std::string_view v = text.substr(j + 1, end - j - 1);
        const auto dot = v.find('.');
        const bool good = dot == std::string_view::npos ? all_digits(v) : all_digits(v.substr(0, dot)) && all_digits(v.substr(dot + 1));
        if (!good && std::find(out.begin(), out.end(), v) == out.end()) out.emplace_back(v);
        i = end + 1;
    }
    return out;
}

bool catalog_versions_harmful(std::string_view gv) {
    if (gv.empty()) return true;
    int major = 0, minor = 0;
    if (std::sscanf(std::string(gv).c_str(), "%d.%d", &major, &minor) != 2) return true;
    return major < 1 || (major == 1 && minor < 7);
}

std::string move_content_catalog_aside(const Instance& inst) {
    const fs::path f = child_ci(appdata_dir(inst), "ContentCatalog.txt");
    std::error_code ec;
    if (f.empty() || !fs::is_regular_file(f, ec)) return {};
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm);
    fs::path bak = f;
    bak += std::string(".mol-bak-") + stamp;
    fs::rename(f, bak, ec);
    if (ec) throw Error("io_error", "cannot move ContentCatalog.txt aside: " + ec.message(), f.string());
    return bak.string();
}

// ---- D2 ----------------------------------------------------------------------------------
std::optional<std::array<int, 4>> pe_file_version(const fs::path& file) {
    const std::string b = read_all(file);
    if (b.size() < 64 || b[0] != 'M' || b[1] != 'Z') return std::nullopt;
    // VS_FIXEDFILEINFO：dwSignature 0xFEEF04BD、dwStrucVersion、dwFileVersionMS、dwFileVersionLS
    static const char sig[] = {'\xBD', '\x04', '\xEF', '\xFE'};
    for (std::size_t i = b.find(std::string_view(sig, 4)); i != std::string::npos; i = b.find(std::string_view(sig, 4), i + 1)) {
        if (i + 16 > b.size()) break;
        if ((le32(b, i + 4) >> 16) != 1) continue;  // dwStrucVersion 0x00010000
        const std::uint32_t ms = le32(b, i + 8), ls = le32(b, i + 12);
        return std::array<int, 4>{static_cast<int>(ms >> 16), static_cast<int>(ms & 0xFFFF), static_cast<int>(ls >> 16), static_cast<int>(ls & 0xFFFF)};
    }
    return std::nullopt;
}

std::string version_string(const std::array<int, 4>& v) {
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]) + "." + std::to_string(v[3]);
}

std::vector<RuntimeDll> vc_runtime(const Instance& inst) {
    std::vector<RuntimeDll> out;
    const fs::path sys = fs::path(std::string(inst.cfg.prefix)) / "drive_c/windows/system32";
    for (const char* n : {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}) {
        RuntimeDll d;
        d.name = n;
        const fs::path f = child_ci(sys, n);
        if (!f.empty()) {
            if (const auto v = pe_file_version(f)) {
                d.version = version_string(*v);
                d.ok = (*v)[0] > 14 || ((*v)[0] == 14 && (*v)[1] >= 40);
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

bool vc_runtime_ok(const std::vector<RuntimeDll>& dlls) {
    return std::all_of(dlls.begin(), dlls.end(), [](const RuntimeDll& d) { return d.ok; });
}

// ---- Steam -------------------------------------------------------------------------------
bool steam_client_running(std::string_view steam_root) {
    std::error_code ec;
    const std::string want = steam_root.empty() ? std::string() : (fs::path(std::string(steam_root)) / "ubuntu12_32/steam").string();
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string pid = it->path().filename().string();
        if (!all_digits(pid)) continue;
        std::error_code e2;
        const fs::path exe = fs::read_symlink(it->path() / "exe", e2);
        if (e2) continue;
        if ((!want.empty() && exe == fs::path(want)) || exe.filename() == "steam") return true;
    }
    return false;
}

bool start_steam_and_wait(std::string_view steam_root, int timeout_ms) {
    const fs::path log = fs::path(std::string(steam_root)) / "logs/connection_log.txt";
    std::error_code ec;
    const auto start_size = fs::exists(log, ec) ? fs::file_size(log, ec) : std::uintmax_t{0};
    LaunchSpec spec;
    const char* path = std::getenv("PATH");
    bool on_path = false;
    for (std::string p = path ? path : ""; !p.empty();) {
        const auto c = p.find(':');
        if (::access((p.substr(0, c) + "/steam").c_str(), X_OK) == 0) { on_path = true; break; }
        if (c == std::string::npos) break;
        p.erase(0, c + 1);
    }
    spec.argv.emplace_back(on_path ? std::string("steam") : (fs::path(std::string(steam_root)) / "steam.sh").string());
    spec.argv.emplace_back("-silent");
    spawn_launch(spec, false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto size = fs::exists(log, ec) ? fs::file_size(log, ec) : std::uintmax_t{0};
        if (size <= start_size) continue;
        std::ifstream in(log, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(start_size));
        const std::string tail((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (tail.find("[Logged On") != std::string::npos && steam_client_running(steam_root)) {
            std::this_thread::sleep_for(std::chrono::seconds(3));  // 登录后客户端还要几秒才接受 SteamAPI 连接
            return true;
        }
    }
    return false;
}

// ---- D4b ---------------------------------------------------------------------------------
std::vector<std::string> case_shadows(const Instance& inst) {
    std::vector<std::string> out;
    const fs::path dir = appdata_dir(inst);
    std::error_code ec;
    for (const char* name : {"plugins.txt", "loadorder.txt"}) {
        if (!fs::exists(fs::symlink_status(dir / name, ec))) continue;  // 还没 sync：不是影子问题（plugins.link 会报）
        const auto want = casefold(name);
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string fn = it->path().filename().string();
            if (fn != name && casefold(fn) == want) out.push_back(it->path().string());
        }
    }
    return out;
}

// ---- SKSE 日志 ----------------------------------------------------------------------------
SkseLog parse_skse_log(std::string_view text) {
    SkseLog log;
    log.found = true;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = text.find('\n', i);
        if (j == std::string_view::npos) j = text.size();
        std::string_view line = text.substr(i, j - i);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        i = j + 1;
        if (line.find("loaded correctly") != std::string_view::npos) ++log.loaded;
        else if (line.find("disabled, fatal error") != std::string_view::npos || line.find("couldn't load plugin") != std::string_view::npos)
            log.failed.push_back(plugin_in_line(line));
        else if (line.find("reported as incompatible") != std::string_view::npos)
            log.incompatible.push_back(plugin_in_line(line));
    }
    return log;
}

SkseLog read_skse_log(const Instance& inst) {
    const fs::path f = resolve_ci(my_games_dir(inst), "SKSE/skse64.log");
    std::error_code ec;
    if (f.empty() || !fs::is_regular_file(f, ec)) return {};
    SkseLog log = parse_skse_log(read_all(f));
    log.mtime = fs::last_write_time(f, ec);
    return log;
}

// ---- D7 ----------------------------------------------------------------------------------
CrashSummary parse_crash_log(std::string_view text) {
    CrashSummary s;
    enum class Sec { None, Cxx, Objects, Stack, StackMem } sec = Sec::None;
    std::size_t frames = 0;
    static const std::set<std::string> sys = {"kernelbase.dll", "kernel32.dll", "ntdll.dll", "ucrtbase.dll", "msvcp140.dll", "vcruntime140.dll",
                                              "vcruntime140_1.dll", "msvcrt.dll", "user32.dll", "win32u.dll"};
    static const char* const exts[] = {".txt", ".ini", ".esp", ".esm", ".esl", ".dll", ".nif", ".dds", ".pex", ".hkx", ".bsa", ".json", ".toml"};
    auto add_unique = [](std::vector<std::string>& v, std::string x) {
        if (!x.empty() && std::find(v.begin(), v.end(), x) == v.end()) v.push_back(std::move(x));
    };
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = text.find('\n', i);
        if (j == std::string_view::npos) j = text.size();
        std::string_view line = text.substr(i, j - i);
        i = j + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        std::string_view t = line;
        while (!t.empty() && (t.front() == '\t' || t.front() == ' ')) t.remove_prefix(1);
        if (s.exception.empty() && line.starts_with("Unhandled exception")) { s.exception = std::string(line); continue; }
        if (line.starts_with("C++ EXCEPTION")) { sec = Sec::Cxx; continue; }
        if (line.starts_with("POSSIBLE RELEVANT OBJECTS")) { sec = Sec::Objects; continue; }
        if (line.starts_with("PROBABLE CALL STACK") || line.starts_with("CALL STACK")) { sec = Sec::Stack; continue; }
        if (line.starts_with("STACK:")) { sec = Sec::StackMem; continue; }
        if (!line.empty() && line.front() != '\t' && line.front() != ' ' && line.back() == ':') { sec = Sec::None; continue; }  // 其它节
        switch (sec) {
            case Sec::Cxx:
                if (t.starts_with("Type: ")) {
                    std::string ty(t.substr(6));
                    if (ty.size() > 2 && ty.front() == '(' && ty.back() == ')') ty = ty.substr(1, ty.size() - 2);
                    s.cxx_type = ty;
                } else if (t.starts_with("Info: ")) {
                    s.cxx_info = std::string(t.substr(6));
                }
                break;
            case Sec::Objects:
                // RSP+868: (PlayerCharacter*) "Adventurer" [0x00000014] ("Constellations - Hard Mode.esp")
                if (const auto p = t.rfind("(\""); p != std::string_view::npos && t.ends_with("\")")) {
                    std::string plug(t.substr(p + 2, t.size() - p - 4));
                    const auto low = casefold(plug);
                    if (low.ends_with(".esp") || low.ends_with(".esm") || low.ends_with(".esl")) add_unique(s.plugins, plug);
                }
                break;
            case Sec::Stack: {
                if (frames >= 12) break;
                if (t.find("[S]") != std::string_view::npos) break;  // 栈扫描得到的帧不可靠，只看 [P]
                const auto plus = t.find('+');
                if (plus == std::string_view::npos) break;
                const auto sp = t.rfind(' ', plus);
                const std::string mod(t.substr(sp == std::string_view::npos ? 0 : sp + 1, plus - (sp == std::string_view::npos ? 0 : sp + 1)));
                const std::string low(casefold(mod));
                if (!(low.ends_with(".dll") || low.ends_with(".exe"))) break;
                ++frames;
                add_unique(s.modules, mod);
                if (s.first_own_frame.empty() && !sys.count(low)) {
                    auto end = t.find_first_of(" \t", plus);
                    s.first_own_frame = std::string(t.substr(sp == std::string_view::npos ? 0 : sp + 1, (end == std::string_view::npos ? t.size() : end) - (sp == std::string_view::npos ? 0 : sp + 1)));
                }
                break;
            }
            case Sec::StackMem: {
                // [RSP+5D0] 0x1350F2C00        (char*) "C:\users\...\ContentCatalog.txt"
                const auto q = t.find("(char*) \"");
                if (q == std::string_view::npos || !t.ends_with("\"")) break;
                const std::string str(t.substr(q + 9, t.size() - q - 10));
                const auto slash = str.find_last_of("\\/");
                const std::string base = slash == std::string::npos ? str : str.substr(slash + 1);
                const std::string low(casefold(base));
                for (const char* e : exts)
                    if (low.size() > std::string_view(e).size() && low.ends_with(e) && base.find(' ') != 0) { add_unique(s.files, base); break; }
                break;
            }
            case Sec::None:
                break;
        }
    }
    // 栈上的残片（"atalog.txt" 是 "ContentCatalog.txt" 的尾巴）不要
    std::erase_if(s.files, [&](const std::string& f) {
        const std::string lf(casefold(f));
        return std::any_of(s.files.begin(), s.files.end(), [&](const std::string& g) {
            const std::string lg(casefold(g));
            return lg.size() > lf.size() && lg.ends_with(lf);
        });
    });
    // 认得出的原因
    for (const auto& f : s.files) {
        if (casefold(f) == "contentcatalog.txt" && (s.cxx_type.find("invalid_argument") != std::string::npos || s.cxx_info.find("sto") != std::string::npos))
            s.hint = "ContentCatalog.txt could not be parsed (usually rewritten by a newer game version): run `fix content-catalog`";
    }
    return s;
}

CrashSummary latest_crash(const Instance& inst) {
    CrashSummary best;
    const fs::path dir = resolve_ci(my_games_dir(inst), "SKSE");
    std::error_code ec;
    fs::path pick;
    fs::file_time_type t{};
    for (fs::directory_iterator it(dir, ec), end; !dir.empty() && !ec && it != end; it.increment(ec)) {
        const std::string n(casefold(it->path().filename().string()));
        if (!n.starts_with("crash-") || !n.ends_with(".log")) continue;
        const auto mt = fs::last_write_time(it->path(), ec);
        if (pick.empty() || mt > t) { pick = it->path(); t = mt; }
    }
    if (pick.empty()) return best;
    best = parse_crash_log(read_all(pick));
    best.file = pick.string();
    best.mtime = t;
    return best;
}

std::string discrete_gpu_unused(const ProtonDiagnosis& d) {
    auto discrete = [](const std::string& n) {
        const std::string l(casefold(n));
        return l.find("nvidia") != std::string::npos || l.find("radeon rx") != std::string::npos || l.find("arc a") != std::string::npos ||
               l.find("arc b") != std::string::npos;
    };
    if (d.gpu_used.empty() || discrete(d.gpu_used)) return {};
    for (const auto& g : d.gpus) if (discrete(g)) return g;
    return {};
}

std::vector<std::string> crash_summary_lines(const CrashSummary& c) {
    std::vector<std::string> out;
    auto join = [](const std::vector<std::string>& v, std::size_t n) {
        std::string s;
        for (std::size_t i = 0; i < v.size() && i < n; ++i) s += (i ? ", " : "") + v[i];
        if (v.size() > n) s += ", …";
        return s;
    };
    if (!c.exception.empty()) out.push_back(c.exception.substr(0, c.exception.find('\t')));
    if (!c.cxx_type.empty() || !c.cxx_info.empty()) out.push_back("C++ exception " + c.cxx_type + (c.cxx_info.empty() ? "" : ": " + c.cxx_info));
    if (!c.first_own_frame.empty()) out.push_back("first non-system frame: " + c.first_own_frame);
    if (!c.files.empty()) out.push_back("files on the stack: " + join(c.files, 4));
    if (!c.plugins.empty()) out.push_back("plugins on the stack: " + join(c.plugins, 4));
    if (!c.hint.empty()) out.push_back("hint: " + c.hint);
    return out;
}

std::string describe_exception_code(std::string_view c) {
    static const std::pair<std::string_view, std::string_view> known[] = {
        {"c0000005", "access violation"}, {"e06d7363", "unhandled C++ exception"}, {"c0000409", "stack buffer overrun / fail-fast"},
        {"c00000fd", "stack overflow"}, {"80000003", "breakpoint"}, {"c0000374", "heap corruption"}, {"c000001d", "illegal instruction"},
        {"c0000094", "integer divide by zero"}, {"c0000135", "DLL not found"}, {"c0000139", "entry point not found"},
    };
    for (const auto& [k, v] : known) if (k == c) return std::string(v);
    return "exception 0x" + std::string(c);
}

ProtonDiagnosis parse_proton_log(std::string_view text) {
    ProtonDiagnosis d;
    std::vector<std::pair<std::uint64_t, std::string>> modules;  // 基址 → 文件名
    struct Pending { std::string thread, code; std::uint64_t addr = 0, info3 = 0; bool open = false; } cur;
    static const std::set<std::string> noise = {"406d1388", "40010006", "4001000a", "6ba", "6be", "6bf", "e24c4a02", "e24c4a03", "80000004"};
    std::map<std::string, std::size_t> counts;
    std::vector<std::string> order;
    auto module_of = [&](std::uint64_t a) -> std::string {
        std::string best;
        std::uint64_t base = 0;
        for (const auto& [b, n] : modules) if (b <= a && b >= base) { base = b; best = n; }
        return best;
    };
    auto finish = [&] {
        if (!cur.open) return;
        cur.open = false;
        if (noise.count(cur.code)) return;
        if (!counts.count(cur.code)) order.push_back(cur.code);
        ++counts[cur.code];
        ProtonException e;
        e.code = cur.code;
        e.thread = cur.thread;
        e.module = module_of(cur.code == "e06d7363" && cur.info3 ? cur.info3 : cur.addr);
        // 崩溃处理器（CrashLogger 写日志、dbghelp 走栈）自己抛的异常不算「最后一个」
        const std::string low(casefold(e.module));
        if (low != "crashlogger.dll" && low != "dbghelp.dll") d.last = e;
    };
    auto hex_after = [](std::string_view line, std::string_view key) -> std::uint64_t {
        const auto p = line.find(key);
        if (p == std::string_view::npos) return 0;
        return std::strtoull(std::string(line.substr(p + key.size(), 16)).c_str(), nullptr, 16);
    };
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = text.find('\n', i);
        if (j == std::string_view::npos) j = text.size();
        const std::string_view line = text.substr(i, j - i);
        i = j + 1;
        if (const auto p = line.find("Loaded L\""); p != std::string_view::npos) {
            const auto q = line.find('"', p + 9);
            const auto at = line.find(" at ", q == std::string_view::npos ? p : q);
            if (q != std::string_view::npos && at != std::string_view::npos) {
                std::string path(line.substr(p + 9, q - p - 9));
                const auto slash = path.find_last_of("\\/");
                modules.emplace_back(hex_after(line, " at "), slash == std::string::npos ? path : path.substr(slash + 1));
            }
            continue;
        }
        // DXVK：适配器列表 "info:  <名字>:"（下一行是 "Driver :"），以及建设备时的 "Device properties:" → "Device : <名字>"
        if (line.starts_with("info:    Device : ")) {
            if (d.gpu_used.empty()) d.gpu_used = std::string(line.substr(18));
            continue;
        }
        if (line.starts_with("info:  ") && line.ends_with(":") && line.size() > 9 && line[7] != ' ' && i < text.size() &&
            text.substr(i).starts_with("info:    Driver : ")) {
            std::string g(line.substr(7, line.size() - 8));
            if (std::find(d.gpus.begin(), d.gpus.end(), g) == d.gpus.end()) d.gpus.push_back(std::move(g));
            continue;
        }
        const auto colon = line.find(':');
        const std::string thread(colon == std::string_view::npos ? std::string_view() : line.substr(0, colon));
        if (line.find(":dispatch_exception code=") != std::string_view::npos) {
            finish();
            const auto p = line.find("code=");
            const auto sp = line.find(' ', p);
            cur = Pending{thread, std::string(line.substr(p + 5, sp - p - 5)), hex_after(line, "addr="), 0, true};
            continue;
        }
        if (cur.open && thread == cur.thread && line.find("dispatch_exception  info[3]=") != std::string_view::npos) cur.info3 = hex_after(line, "info[3]=");
    }
    finish();
    for (const auto& c : order) d.codes.emplace_back(c, counts[c]);
    return d;
}

// ---- D3 ----------------------------------------------------------------------------------
std::string enb_wanted_by(const Instance& inst) {
    for (const char* n : {"enblocal.ini", "enbseries"})
        if (!child_ci(fs::path(std::string(inst.cfg.game_dir)), n).empty()) return std::string("game folder: ") + n;
    for (const auto& m : list_mods(inst)) {
        if (!m.enabled || !m.exists || m.separator) continue;
        const fs::path base(std::string(m.path));
        for (const char* n : {"enblocal.ini", "enbseries"})
            if (!child_ci(base, n).empty()) return std::string(m.name) + ": " + n;
        // ENB 的辅助插件：SKSE 插件目录或 KiLoader 的插件目录
        for (const char* dir : {"SKSE/Plugins", "KiLoader/Plugins"}) {
            const fs::path plugins = resolve_ci(base, m.root ? std::string("Data/") + dir : std::string(dir));
            if (plugins.empty()) continue;
            for (const char* n : {"ENBHelperSE.dll", "KiENBExtender.dll"})
                if (!child_ci(plugins, n).empty()) return std::string(m.name) + ": " + n;
        }
    }
    return {};
}

bool enb_binaries_present(const Instance& inst) {
    if (!child_ci(fs::path(std::string(inst.cfg.game_dir)), "d3d11.dll").empty()) return true;
    for (const auto& m : list_mods(inst))
        if (m.enabled && m.exists && !m.separator && m.root && !child_ci(fs::path(std::string(m.path)), "d3d11.dll").empty()) return true;
    return false;
}

// ---- P0-3 --------------------------------------------------------------------------------
std::vector<FileHit> find_file(const Instance& inst, std::string_view file_name, bool archives) {
    std::vector<FileHit> out;
    const auto want = casefold(file_name);
    std::error_code ec;
    for (const auto& m : list_mods(inst)) {
        if (m.separator || !m.exists) continue;
        const fs::path base(std::string(m.path));
        for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
            if (casefold(it->path().filename().string()) == want && it->is_regular_file(ec))
                out.push_back({std::string(m.name), it->path().lexically_relative(base).generic_string(), false, m.enabled});
        ec.clear();
    }
    if (archives) {
        for (fs::directory_iterator it(fs::path(std::string(inst.downloads_dir)), ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto ext = casefold(it->path().extension().string());
            if (ext != ".7z" && ext != ".zip" && ext != ".rar") continue;
            std::vector<std::string> names;
            try { names = list_archive(it->path().string()); } catch (const Error&) { continue; }
            for (const auto& n : names) {
                const auto slash = n.find_last_of('/');
                if (casefold(slash == std::string::npos ? std::string_view(n) : std::string_view(n).substr(slash + 1)) == want)
                    out.push_back({it->path().filename().string(), n, true, true});
            }
        }
    }
    return out;
}

// ---- D4 ----------------------------------------------------------------------------------
std::vector<LayoutIssue> scan_mod_layouts(const Instance& inst) {
    std::vector<LayoutIssue> out;
    std::error_code ec;
    for (const auto& m : list_mods(inst)) {
        if (m.separator || !m.exists) continue;
        const fs::path dir(std::string(m.path));
        bool has_data = false, has_config = false, has_enb = false;
        std::string backslash;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string fn = it->path().filename().string();
            const auto low = casefold(fn);
            if (fn.find('\\') != std::string::npos && backslash.empty()) backslash = fn;
            if (low == "data" && it->is_directory(ec)) has_data = true;
            if (low == "moduleconfig.xml") has_config = true;
            if (low == "fomod" && it->is_directory(ec) && !child_ci(it->path(), "ModuleConfig.xml").empty()) has_config = true;  // 整包原样装进来了
            if (low == "enbseries" || low == "enblocal.ini" || low == "enbseries.ini") has_enb = true;
        }
        ec.clear();
        const std::string name(m.name);
        if (!backslash.empty()) out.push_back({name, "backslash_name", backslash});
        if (has_config) out.push_back({name, "raw_fomod", "a FOMOD config sits inside the mod: the FOMOD installer was never run"});
        if (has_enb && !m.root) out.push_back({name, "enb_in_data", "an ENB preset installed as a normal mod lands in Data/, but ENB reads it from the game folder"});
        if (has_data && !m.root && !is_data_root_dir(dir.string()))
            out.push_back({name, "nested_data", "the content sits in Data/ inside the mod, so the game never sees it"});
    }
    return out;
}

}  // namespace mol::health
