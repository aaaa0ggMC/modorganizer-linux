#include "mol/doctor.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <initializer_list>
#include <string>
#include <system_error>

#include "mol/casefold.hpp"
#include "mol/health.hpp"
#include "mol/mo2fmt.hpp"
#include "mol/overwrite.hpp"
#include "mol/plugins.hpp"

namespace mol {
namespace fs = std::filesystem;
namespace {

bool exists_ci(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    const auto want = casefold(name);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (casefold(it->path().filename().string()) == want) return true;
    return false;
}

// 游戏目录或任一已启用的「根目录型」mod 里是否有该文件（顶层，大小写不敏感）。
bool provided_at_root(const Instance& inst, const fs::path& game, std::string_view name, mr* mem) {
    if (exists_ci(game, name)) return true;
    for (const auto& m : list_mods(inst, {}, mem))
        if (m.enabled && m.exists && m.root && exists_ci(fs::path(std::string(m.path)), name)) return true;
    return false;
}

}  // namespace

bool root_provides(const Instance& inst, std::string_view name, mr* mem) {
    return provided_at_root(inst, fs::path(std::string(inst.cfg.game_dir)), name, mem);
}

namespace {
struct Sink {
    vector<Check>& out;
    mr* mem;
    void add(std::string_view id, std::string_view level, std::string_view msg, std::string_view hint = {}, std::initializer_list<std::string_view> fix = {}) {
        Check c(mem);
        for (auto f : fix) c.fix.push_back(string(f, mem));
        c.id = string(id, mem);
        c.level = string(level, mem);
        c.message = string(msg, mem);
        c.hint = string(hint, mem);
        out.push_back(std::move(c));
    }
};
}  // namespace

namespace {
std::string join_some(const std::vector<std::string>& v, std::size_t n = 5) {
    std::string out;
    for (std::size_t i = 0; i < v.size() && i < n; ++i) out += (i ? ", " : "") + v[i];
    if (v.size() > n) out += ", … (" + std::to_string(v.size()) + " in total)";
    return out;
}

void prefix_health(Sink& s, const Instance& inst, std::string_view game_version) {
    namespace h = health;
    std::error_code ec;
    // D1：被 1.7.x 写坏的 ContentCatalog.txt（1.6.x 启动约 8 秒后静默退出）
    {
        const fs::path f = h::appdata_dir(inst) / "ContentCatalog.txt";
        std::ifstream in(f, std::ios::binary);
        if (in) {
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto bad = h::bad_catalog_versions(text);
            if (!bad.empty() && h::catalog_versions_harmful(game_version))
                s.add("game.content_catalog", "error",
                      "ContentCatalog.txt has entries this game version cannot parse (" + join_some(bad, 2) + "); the game exits a few seconds after start",
                      "it was rewritten by a newer game version (e.g. Steam ran 1.7.x); moving it aside is safe, the game rebuilds it", {"fix", "content-catalog"});
            else
                s.add("game.content_catalog", "ok", "ContentCatalog.txt is readable by this game version");
        }
    }
    // D2：VC++ 运行库
    const auto rt = h::vc_runtime(inst);
    if (h::vc_runtime_ok(rt)) {
        s.add("prefix.vcrun", "ok", "VC++ runtime " + rt.front().version);
    } else {
        std::string have;
        for (const auto& d : rt) have += (have.empty() ? "" : ", ") + d.name + " " + (d.version.empty() ? std::string("missing") : d.version);
        s.add("prefix.vcrun", "error", "the prefix's VC++ runtime is too old (" + have + "); most current SKSE plugins need 14.40 or newer and fail to load",
              "installs Microsoft's VC++ 2015-2022 redistributable into the prefix", {"fix", "vcrun"});
    }
    // D4b：plugins.txt 的大小写影子
    if (const auto sh = h::case_shadows(inst); !sh.empty()) {
        std::vector<std::string> names;
        for (const auto& p : sh) names.push_back(fs::path(p).filename().string());
        s.add("prefix.case_shadows", "error",
              "the prefix has " + join_some(names) + " next to our link; Wine opens that file instead, so the game ignores the profile's plugin list",
              "`plugins sync` moves it aside as .mol-backup", {"plugins", "sync"});
    }
    // 上次运行的 SKSE 日志
    const auto log = h::read_skse_log(inst);
    if (log.found && !log.failed.empty()) {
        // 日志比运行库还旧 → 是修复之前的那次运行，不再提
        const fs::path msvcp = fs::path(std::string(inst.cfg.prefix)) / "drive_c/windows/system32/msvcp140.dll";
        const auto rt_time = fs::last_write_time(msvcp, ec);
        if (ec || log.mtime >= rt_time)
            s.add("skse.plugins", "warn",
                  std::to_string(log.failed.size()) + " SKSE plugin(s) failed to load in the last run (" + join_some(log.failed, 4) + ")",
                  h::vc_runtime_ok(rt) ? "see `logs --file SKSE/skse64.log`; a plugin built for another game version or missing its dependency" : "usually the old VC++ runtime: run `fix vcrun`");
    } else if (log.found) {
        s.add("skse.plugins", "ok", std::to_string(log.loaded) + " SKSE plugin(s) loaded in the last run");
    }
}

void mods_health(Sink& s, const Instance& inst) {
    namespace h = health;
    // D3：要 ENB 但没有 ENB 二进制
    if (const auto why = h::enb_wanted_by(inst); !why.empty() && !h::enb_binaries_present(inst))
        s.add("enb.binaries", "warn", "the mod list expects ENB (" + why + ") but there is no ENB d3d11.dll; ENB effects and ENB-dependent plugins are off",
              "download the ENB for Skyrim SE from enbdev.com (browser only), then `enb install --archive <the zip>`");
    // D4：旧版本装坏的 mod 布局
    std::map<std::string, std::vector<std::string>> by_kind;
    for (const auto& i : h::scan_mod_layouts(inst)) by_kind[i.kind].push_back(i.mod);
    static const char* const reinstall = "reinstall them: collection mods with `collection resolve SLUG --mod KEY --reinstall` then `collection install SLUG`; others with `mods install` from their archive";
    struct Kind { const char* id; const char* what; const char* hint; };
    static const Kind kinds[] = {
        {"nested_data", "content sits in a Data/ folder inside the mod, so the game never sees it", reinstall},
        {"raw_fomod", "ModuleConfig.xml at the top level: the FOMOD installer was never run", reinstall},
        {"backslash_name", "file names contain '\\' (a Windows zip unpacked literally)", reinstall},
        {"enb_in_data", "an ENB preset installed as a normal mod lands in Data/, where ENB never looks",
         "collection mods: `collection install SLUG` marks them as root-folder mods (no reinstall); others: reinstall with `mods install --root`"},
    };
    for (const auto& k : kinds) {
        const auto it = by_kind.find(k.id);
        if (it == by_kind.end()) continue;
        s.add(std::string("mods.layout.") + k.id, "warn", std::to_string(it->second.size()) + " mod(s): " + k.what + ": " + join_some(it->second), k.hint);
    }
}
}  // namespace

string skse_dll_name(std::string_view v, mr* mem) {
    int part[4] = {-1, -1, -1, -1};
    int n = 0;
    std::size_t i = 0;
    while (i < v.size() && n < 4) {
        std::size_t j = i;
        while (j < v.size() && v[j] >= '0' && v[j] <= '9') ++j;
        if (j == i) return string(mem);
        part[n++] = std::stoi(std::string(v.substr(i, j - i)));
        if (j < v.size() && v[j] != '.') return string(mem);
        i = j + 1;
    }
    if (n < 3) return string(mem);
    return string("skse64_" + std::to_string(part[0]) + "_" + std::to_string(part[1]) + "_" + std::to_string(part[2]) + ".dll", mem);
}

vector<Check> run_doctor(const Instance& inst, std::string_view game_version, mr* mem) {
    vector<Check> out(mem);
    Sink s{out, mem};
    std::error_code ec;
    const fs::path game{std::string(inst.cfg.game_dir)};

    // 游戏本体
    if (inst.cfg.game_dir.empty() || !fs::is_directory(game, ec)) {
        s.add("game.dir", "error", "game directory missing: " + std::string(inst.cfg.game_dir), "run `instance init --game-dir`");
    } else {
        s.add("game.dir", "ok", "game directory: " + std::string(inst.cfg.game_dir));
        if (!exists_ci(game, "SkyrimSE.exe")) s.add("game.exe", "error", "SkyrimSE.exe not found in the game directory");
        else s.add("game.exe", "ok", "SkyrimSE.exe present");
        if (!exists_ci(game, "Data")) s.add("game.data", "error", "Data directory not found in the game directory");
        if (game_version.empty())
            s.add("game.version", "warn", "game version unknown (game host library unavailable)", "build with MOL_BUILD_HOST=ON or set MOL_GAME_LIB");
        else
            s.add("game.version", "ok", "game version " + std::string(game_version));
    }

    // SKSE
    if (!game.empty() && fs::is_directory(game, ec)) {
        const bool loader = provided_at_root(inst, game, "skse64_loader.exe", mem);
        if (!loader) {
            s.add("skse.loader", "warn", "SKSE64 is not installed (skse64_loader.exe missing)", "optional; needed by most script mods", {"skse", "install"});
        } else if (game_version.empty()) {
            s.add("skse.version", "warn", "SKSE64 installed, cannot compare with the game version");
        } else {
            const string dll = skse_dll_name(game_version);
            if (dll.empty()) s.add("skse.version", "warn", "cannot derive the SKSE runtime name from game version " + std::string(game_version));
            else if (provided_at_root(inst, game, dll, mem)) s.add("skse.version", "ok", "SKSE64 runtime " + std::string(dll) + " matches the game");
            else s.add("skse.version", "error", "SKSE64 does not match the game: " + std::string(dll) + " not found; the loader will refuse to start",
                       "install the SKSE64 build for game version " + std::string(game_version), {"skse", "install"});
        }
    }

    // 前缀 / Proton
    const fs::path prefix{std::string(inst.cfg.prefix)};
    if (inst.cfg.prefix.empty() || !fs::is_directory(prefix / "drive_c", ec))
        s.add("prefix", "error", "Wine prefix missing or has no drive_c: " + std::string(inst.cfg.prefix), "start the game once from Steam to create it");
    else
        s.add("prefix", "ok", "prefix: " + std::string(inst.cfg.prefix));
    if (inst.cfg.runner_kind == "proton") {
        if (inst.cfg.proton_path.empty() || !fs::exists(fs::path(std::string(inst.cfg.proton_path)) / "proton", ec))
            s.add("runner.proton", "error", "Proton script not found: " + std::string(inst.cfg.proton_path), "set --proton-path");
        else
            s.add("runner.proton", "ok", "Proton: " + std::string(inst.cfg.proton_path));
        if (inst.cfg.steam_root.empty() || !fs::is_directory(fs::path(std::string(inst.cfg.steam_root)), ec))
            s.add("runner.steam_root", "warn", "Steam root missing: " + std::string(inst.cfg.steam_root));
    }

    // 前缀里的疑难杂症（docs/PLAN-autofix.md D1/D2/D4b，SKSE 日志）
    if (!inst.cfg.prefix.empty() && fs::is_directory(prefix / "drive_c", ec)) prefix_health(s, inst, game_version);

    // profile / mods
    const fs::path pdir = fs::path(std::string(inst.profiles_dir)) / std::string(inst.cfg.profile);
    if (!fs::is_directory(pdir, ec)) {
        s.add("profile", "error", "profile directory missing: " + pdir.string());
    } else {
        s.add("profile", "ok", "profile " + std::string(inst.cfg.profile));
        std::size_t missing = 0, enabled = 0;
        for (const auto& m : list_mods(inst, {}, mem)) {
            if (m.separator || !m.enabled) continue;
            ++enabled;
            if (!m.exists) {
                ++missing;
                s.add("mods.missing", "warn", "enabled mod has no directory: " + std::string(m.name));
            }
        }
        if (missing == 0) s.add("mods", "ok", std::to_string(enabled) + " enabled mod(s), all present");
    }

    // 农场
    if (!inst.cfg.game_dir.empty() && fs::is_directory(game, ec)) {
        try {
            const FarmModel model = build_farm_model(inst, {}, mem);
            const Plan plan = plan_instance(inst, model, mem);
            if (!model.merged.warnings.empty())
                s.add("farm.warnings", "warn", std::to_string(model.merged.warnings.size()) + " merge warning(s) (case conflicts)", "see `plan` warnings");
            if (!fs::exists(fs::path(std::string(inst.farm_path)) / kFarmMarker, ec))
                s.add("farm", "warn", "farm not created yet", "run `apply`", {"apply"});
            else if (plan.ops.empty())
                s.add("farm", "ok", "farm in sync");
            else
                s.add("farm", "warn", "farm is out of sync: " + std::to_string(plan.ops.size()) + " pending op(s)", "run `apply`", {"apply"});
        } catch (const Error& e) {
            s.add("farm", "error", e.what(), e.code);
        }
        if (const std::string who = farm_user(inst); !who.empty())
            s.add("farm.busy", "warn", "a process is using the farm (" + who + ")", "if the game is not running, these are leftovers: `terminate` ends them", {"terminate"});
    }

    // 插件依赖（masters）
    if (fs::is_directory(pdir, ec) && !inst.cfg.game_dir.empty() && fs::is_directory(game, ec)) {
        try {
            const PluginList pl = load_plugins(inst, {}, {}, mem);
            const auto issues = check_masters(pl, mem);
            if (issues.empty()) {
                s.add("plugins.masters", "ok", std::to_string(pl.rows.size()) + " plugin(s), all masters satisfied");
            } else {
                for (const auto& is : issues) {
                    const std::string k(is.kind);
                    if (k == "missing")
                        s.add("plugins.masters", "error", std::string(is.plugin) + " requires " + std::string(is.master) + ", which is not installed",
                              "install the missing master or disable " + std::string(is.plugin));
                    else if (k == "disabled")
                        s.add("plugins.masters", "error", std::string(is.plugin) + " requires " + std::string(is.master) + ", which is disabled",
                              "enable " + std::string(is.master), {"plugins", "enable", std::string_view(is.master)});
                    else
                        s.add("plugins.masters", "warn", std::string(is.plugin) + " is loaded before its master " + std::string(is.master), "run `plugins sort`", {"plugins", "sort"});
                }
            }
        } catch (const Error& e) {
            s.add("plugins.masters", "warn", std::string("could not check plugin masters: ") + e.what());
        }
    }

    if (fs::is_directory(fs::path(std::string(inst.mods_dir)), ec)) mods_health(s, inst);

    // plugins.txt 映射（只检查最常见的位置，不创建）
    const fs::path appdata = prefix / "drive_c/users" / std::string(inst.cfg.prefix_user) / "AppData/Local/Skyrim Special Edition/plugins.txt";
    const auto st = fs::symlink_status(appdata, ec);
    if (fs::is_symlink(st)) s.add("plugins.link", "ok", "plugins.txt is linked to the profile");
    else if (fs::exists(st)) s.add("plugins.link", "warn", "plugins.txt in the prefix is a real file, not linked to the profile", "run `plugins sync` (the file is kept as .mol-backup)", {"plugins", "sync"});
    else s.add("plugins.link", "warn", "plugins.txt is not linked yet", "run `plugins sync`", {"plugins", "sync"});
    return out;
}

}  // namespace mol
