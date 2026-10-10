#include "mol/rules.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "mol/impact.hpp"
#include "mol/instance.hpp"
#include "mol/mo2fmt.hpp"

namespace mol::rules {
namespace fs = std::filesystem;
namespace {

// Normalize a boolean-ish rules.ini value to the canonical lowercase "true"/"false"
// string that the rule layer and the disable_mod guard compare against. Recognized
// truthy tokens are matched case-insensitively; any other present value is treated
// as false. An absent key never reaches here (see add_pref_fact), so a missing
// preference is never defaulted to true.
string normalize_bool(std::string_view raw, mr* mem) {
    std::string folded;
    folded.reserve(raw.size());
    for (char c : raw) folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (folded == "true" || folded == "1" || folded == "yes" || folded == "on") return string("true", mem);
    return string("false", mem);
}

// Emit preferences.<fact_key> from rules.ini [Preferences] <ini_key>, but only when
// the key is actually present. Missing keys produce no fact at all (no default), so
// callers cannot mistake an absent preference for an explicit "true".
void add_pref_fact(vector<Fact>& facts, const Ini& ini, std::string_view ini_key,
                   std::string_view fact_key, mr* mem) {
    if (auto v = ini.get("Preferences", ini_key, mem)) {
        Fact f(mem);
        f.key = string(fact_key, mem);
        f.value = normalize_bool(*v, mem);
        facts.push_back(std::move(f));
    }
}

// Bounded tail read: at most max_bytes from the end of the file. Returns false on any
// I/O error so the caller can treat the read as an isolated failure and simply omit
// the fact instead of aborting the whole collection.
bool read_tail(const fs::path& file, std::size_t max_bytes, string& out, mr* /*mem*/) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const std::uintmax_t start = size > max_bytes ? size - static_cast<std::uintmax_t>(max_bytes) : 0;
    in.seekg(static_cast<std::streamoff>(start));
    if (!in) return false;
    out.resize(max_bytes);
    in.read(out.data(), static_cast<std::streamsize>(max_bytes));
    out.resize(static_cast<std::size_t>(in.gcount()));
    if (in.bad()) return false;  // a genuine read error, not a normal EOF
    return true;
}

}  // namespace

// Build the read-only fact snapshot the Lua rules consume. Pure collection: no fixes,
// no process/package access. The host (mol::rules::run) is responsible for evaluation.
Context collect_context(const Instance& inst, std::string_view game_version, mr* mem) {
    Context ctx(mem);
    ctx.game = string(inst.cfg.game, mem);
    ctx.game_version = string(game_version, mem);
    ctx.mods = list_mods(inst, {}, mem);
    // 影响面事实（带磁盘缓存）。收集失败不能让体检整体失败：rules 没有 impact 也能跑。
    try {
        ctx.impact = impact::collect_impact(inst, mem);
    } catch (const std::exception&) {
    }

    // Player-facing preferences live in the profile's rules.ini [Preferences] section.
    // Ini::load returns an empty Ini when the file is absent, so a profile without a
    // rules.ini simply contributes no preference facts.
    const fs::path rules_ini =
        fs::path(std::string(inst.profiles_dir)) / std::string(inst.cfg.profile) / "rules.ini";
    const Ini ini = Ini::load(rules_ini.string(), mem);
    add_pref_fact(ctx.facts, ini, "ShowPlayerWorldmapPosition", "preferences.show_player_worldmap_position", mem);
    add_pref_fact(ctx.facts, ini, "AllowDisableMod", "preferences.allow_disable_mod", mem);

    // Skyrim only: expose the ENB Extender compiler log as evidence, but only when it
    // belongs to the current launch. We treat the SKSE loader log as the launch marker:
    // the ENB log is attached only if it is at least as new as that marker. Without the
    // marker (no SKSE launch recorded) the ENB log is stale from an earlier run and must
    // not be read as a current-session fault. Only the single, current KiENBExtender.log is
    // read (no rotated *.1/*.2 logs), and its read is bounded and error-isolated.
    if (inst.cfg.game == "skyrimse" && !inst.cfg.prefix.empty()) {
        // Fixed, non-arbitrary relative locations inside the Wine prefix; no path is
        // ever taken from Lua or user input.
        const fs::path base =
            fs::path(std::string(inst.cfg.prefix)) / "drive_c/users" / std::string(inst.cfg.prefix_user);
        const fs::path enb_log = base / "AppData/Local/KiLoader/SkyrimSE/Logs/KiENBExtender.log";
        const fs::path skse_loader = base / "Documents/My Games/Skyrim Special Edition/SKSE/skse64_loader.log";

        std::error_code enb_ec;
        const auto enb_time = fs::last_write_time(enb_log, enb_ec);
        if (!enb_ec) {
            std::error_code skse_ec;
            const auto skse_time = fs::last_write_time(skse_loader, skse_ec);
            // Absent launch marker => no current compiler fact.
            if (!skse_ec && enb_time >= skse_time) {
                string tail(mem);
                if (read_tail(enb_log, 64 * 1024, tail, mem)) {
                    Fact f(mem);
                    f.key = string("enb.compiler_log", mem);
                    f.value = std::move(tail);
                    ctx.facts.push_back(std::move(f));
                }
            }
        }
    }
    return ctx;
}

}  // namespace mol::rules
