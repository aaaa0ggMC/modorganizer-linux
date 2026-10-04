#include "mol/doctor.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#include "mol/casefold.hpp"
#include "mol/mo2fmt.hpp"
#include "mol/overwrite.hpp"

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

struct Sink {
    vector<Check>& out;
    mr* mem;
    void add(std::string_view id, std::string_view level, std::string_view msg, std::string_view hint = {}) {
        Check c(mem);
        c.id = string(id, mem);
        c.level = string(level, mem);
        c.message = string(msg, mem);
        c.hint = string(hint, mem);
        out.push_back(std::move(c));
    }
};
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
        const bool loader = exists_ci(game, "skse64_loader.exe");
        if (!loader) {
            s.add("skse.loader", "warn", "SKSE64 is not installed (skse64_loader.exe missing)", "optional; needed by most script mods");
        } else if (game_version.empty()) {
            s.add("skse.version", "warn", "SKSE64 installed, cannot compare with the game version");
        } else {
            const string dll = skse_dll_name(game_version);
            if (dll.empty()) s.add("skse.version", "warn", "cannot derive the SKSE runtime name from game version " + std::string(game_version));
            else if (exists_ci(game, dll)) s.add("skse.version", "ok", "SKSE64 runtime " + std::string(dll) + " matches the game");
            else s.add("skse.version", "error", "SKSE64 does not match the game: " + std::string(dll) + " not found; the loader will refuse to start",
                       "install the SKSE64 build for game version " + std::string(game_version));
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
                s.add("farm", "warn", "farm not created yet", "run `apply`");
            else if (plan.ops.empty())
                s.add("farm", "ok", "farm in sync");
            else
                s.add("farm", "warn", "farm is out of sync: " + std::to_string(plan.ops.size()) + " pending op(s)", "run `apply`");
        } catch (const Error& e) {
            s.add("farm", "error", e.what(), e.code);
        }
        if (farm_in_use(inst)) s.add("farm.busy", "warn", "a process is using the farm (game running?)");
    }

    // plugins.txt 映射（只检查最常见的位置，不创建）
    const fs::path appdata = prefix / "drive_c/users" / std::string(inst.cfg.prefix_user) / "AppData/Local/Skyrim Special Edition/plugins.txt";
    const auto st = fs::symlink_status(appdata, ec);
    if (fs::is_symlink(st)) s.add("plugins.link", "ok", "plugins.txt is linked to the profile");
    else if (fs::exists(st)) s.add("plugins.link", "warn", "plugins.txt in the prefix is a real file, not linked to the profile", "run `plugins sync` (the file is kept as .mol-backup)");
    else s.add("plugins.link", "warn", "plugins.txt is not linked yet", "run `plugins sync`");
    return out;
}

}  // namespace mol
