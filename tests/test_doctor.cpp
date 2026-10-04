#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/doctor.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
std::string level_of(const vector<Check>& cs, std::string_view id) {
    for (const auto& c : cs) if (c.id == id) return std::string(c.level);
    return "<none>";
}
}  // namespace

TEST(skse_dll_name_from_game_version) {
    CHECK_EQ(std::string(skse_dll_name("1.6.1170.0")), std::string("skse64_1_6_1170.dll"));
    CHECK_EQ(std::string(skse_dll_name("1.7.104.0")), std::string("skse64_1_7_104.dll"));
    CHECK(skse_dll_name("garbage").empty());
    CHECK(skse_dll_name("").empty());
    CHECK(skse_dll_name("1.2").empty());
}

TEST(doctor_flags_skse_mismatch_and_missing_prefix) {
    const fs::path t = fs::temp_directory_path() / ("mol_doc_" + std::to_string(::getpid()));
    fs::remove_all(t);
    touch(t / "game/SkyrimSE.exe");
    touch(t / "game/Data/Skyrim.esm");
    touch(t / "game/skse64_loader.exe");
    touch(t / "game/skse64_1_6_1170.dll");
    fs::create_directories(t / "inst/profiles/Default");
    Instance i;
    i.root.assign((t / "inst").string());
    i.profiles_dir.assign((t / "inst/profiles").string());
    i.mods_dir.assign((t / "inst/mods").string());
    i.overwrite_dir.assign((t / "inst/overwrite").string());
    i.farm_path.assign((t / "inst/farm").string());
    i.cfg.game_dir.assign((t / "game").string());
    i.cfg.prefix.assign((t / "nopfx").string());
    i.cfg.prefix_user.assign("steamuser");
    i.cfg.profile.assign("Default");
    i.cfg.runner_kind.assign("wine");

    CHECK_EQ(level_of(run_doctor(i, "1.7.104.0"), "skse.version"), std::string("error"));
    CHECK_EQ(level_of(run_doctor(i, "1.6.1170.0"), "skse.version"), std::string("ok"));
    CHECK_EQ(level_of(run_doctor(i, ""), "skse.version"), std::string("warn"));
    CHECK_EQ(level_of(run_doctor(i, "1.6.1170.0"), "prefix"), std::string("error"));
    CHECK_EQ(level_of(run_doctor(i, "1.6.1170.0"), "game.exe"), std::string("ok"));
    fs::remove_all(t);
}
