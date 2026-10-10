#include "minitest.hpp"
#include "mol/rules.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
namespace fs = std::filesystem;
namespace {
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("mol_rules_facts_" + std::to_string(getpid()));
    mol::Instance inst;
    Fixture() {
        fs::remove_all(root);
        fs::create_directories(root / "profiles/Default");
        fs::create_directories(root / "mods");
        inst.root = root.string();
        inst.profiles_dir = (root / "profiles").string();
        inst.mods_dir = (root / "mods").string();
        inst.cfg.profile = "Default";
        inst.cfg.game = "skyrimse";
        inst.cfg.prefix = (root / "prefix").string();
        inst.cfg.prefix_user = "steamuser";
    }
    ~Fixture() { fs::remove_all(root); }
};
std::string fact(const mol::rules::Context &c, std::string_view key) {
    for (auto &f : c.facts)
        if (f.key == key)
            return std::string(f.value);
    return {};
}
void write(const fs::path &p, std::string_view s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}
} // namespace
TEST(compiler_diagnostics_require_launch_marker_and_fresh_log) {
    Fixture f;
    auto user = f.root / "prefix/drive_c/users/steamuser";
    auto log = user / "AppData/Local/KiLoader/SkyrimSE/Logs/KiENBExtender.log";
    auto launch = user / "Documents/My Games/Skyrim Special Edition/SKSE/skse64_loader.log";
    write(log, "E5020 fx_5_0");
    CHECK(fact(mol::rules::collect_context(f.inst, "1.6.1170"), "enb.compiler_log").empty());
    write(launch, "started");
    auto now = fs::file_time_type::clock::now();
    fs::last_write_time(launch, now);
    fs::last_write_time(log, now - std::chrono::seconds(1));
    CHECK(fact(mol::rules::collect_context(f.inst, "1.6.1170"), "enb.compiler_log").empty());
    fs::last_write_time(log, now + std::chrono::seconds(1));
    CHECK(fact(mol::rules::collect_context(f.inst, "1.6.1170"), "enb.compiler_log").find("E5020") != std::string::npos);
    auto ctx = mol::rules::collect_context(f.inst, "1.6.1170");
    auto rows = mol::rules::evaluate(mol::rules::builtin_sources(), ctx);
    bool reported = false;
    for (auto &r : rows)
        if (r.id == "lua.skyrim.compiler_target")
            reported = true;
    CHECK(reported);
}
TEST(map_preference_is_opt_in_and_proposes_native_disable_command) {
    Fixture f;
    fs::create_directories(f.root / "mods/Get Lost for Anniversary Edition");
    write(f.root / "profiles/Default/modlist.txt", "+Get Lost for Anniversary Edition\n");
    auto ctx = mol::rules::collect_context(f.inst, "1.6.1170");
    CHECK(fact(ctx, "preferences.allow_disable_mod") != "true");
    auto rows = mol::rules::evaluate(mol::rules::builtin_sources(), ctx);
    CHECK(!rows.empty());
    for (auto &r : rows)
        CHECK(r.fix.empty());
    write(f.root / "profiles/Default/rules.ini",
          "[Preferences]\nShowPlayerWorldmapPosition=true\nAllowDisableMod=true\n");
    ctx = mol::rules::collect_context(f.inst, "1.6.1170");
    CHECK(fact(ctx, "preferences.show_player_worldmap_position") == "true");
    rows = mol::rules::evaluate(mol::rules::builtin_sources(), ctx);
    bool found = false;
    for (auto &r : rows)
        if (!r.fix.empty()) {
            CHECK_EQ(r.fix.size(), 3u);
            CHECK(r.fix[0] == "mods");
            CHECK(r.fix[1] == "disable");
            CHECK(r.fix[2] == "Get Lost for Anniversary Edition");
            found = true;
        }
    CHECK(found);
}
