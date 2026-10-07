#include "mol/health.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

#include "mol/casefold.hpp"
#include "mol/mo2fmt.hpp"
#include "mol/mod_install.hpp"

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
            if (low == "enbseries" || low == "enblocal.ini" || low == "enbseries.ini") has_enb = true;
        }
        ec.clear();
        const std::string name(m.name);
        if (!backslash.empty()) out.push_back({name, "backslash_name", backslash});
        if (has_config) out.push_back({name, "raw_fomod", "ModuleConfig.xml at the top level: the FOMOD installer was never run"});
        if (has_enb && !m.root) out.push_back({name, "enb_in_data", "an ENB preset installed as a normal mod lands in Data/, but ENB reads it from the game folder"});
        if (has_data && !m.root && !is_data_root_dir(dir.string()))
            out.push_back({name, "nested_data", "the content sits in Data/ inside the mod, so the game never sees it"});
    }
    return out;
}

}  // namespace mol::health
