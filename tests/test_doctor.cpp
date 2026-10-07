#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

namespace {
// 最小的 TES4 插件头：flags + masters
void plugin(const fs::path& p, std::uint32_t flags, const std::vector<std::string>& masters) {
    std::string data;
    auto sub = [&](const char* type, const std::string& body) {
        data.append(type, 4);
        data.push_back(static_cast<char>(body.size() & 0xFF));
        data.push_back(static_cast<char>(body.size() >> 8));
        data += body;
    };
    sub("HEDR", std::string(12, '\0'));
    for (const auto& m : masters) {
        sub("MAST", m + std::string(1, '\0'));
        sub("DATA", std::string(8, '\0'));
    }
    std::string rec = "TES4";
    auto le32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) rec.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); };
    le32(static_cast<std::uint32_t>(data.size()));
    le32(flags);
    le32(0); le32(0);
    rec.push_back(44); rec.push_back(0); rec.push_back(0); rec.push_back(0);
    rec += data;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << rec;
}
std::string msg_of(const vector<Check>& cs, std::string_view id) {
    for (const auto& c : cs) if (c.id == id) return std::string(c.message) + " | " + std::string(c.hint);
    return "<none>";
}
std::string fix_of(const vector<Check>& cs, std::string_view id) {
    for (const auto& c : cs) if (c.id == id) { std::string f; for (const auto& x : c.fix) f += std::string(x) + " "; return f; }
    return "<none>";
}
}  // namespace

TEST(doctor_reports_prefix_troubles_and_where_a_missing_master_hides) {
    const fs::path t = fs::temp_directory_path() / ("mol_doc2_" + std::to_string(::getpid()));
    fs::remove_all(t);
    touch(t / "game/SkyrimSE.exe");
    plugin(t / "game/Data/Skyrim.esm", 0x1, {});
    // 前缀：旧运行库、被 1.7 写坏的 ContentCatalog、Plugins.txt 大小写影子
    const fs::path ad = t / "pfx/drive_c/users/steamuser/AppData/Local/Skyrim Special Edition";
    touch(t / "pfx/drive_c/windows/system32/msvcp140.dll");  // 读不到版本 = 不合格
    fs::create_directories(ad);
    std::ofstream(ad / "ContentCatalog.txt") << R"({"a":{"Version" : "1701307962.== Version Number =="}})";
    std::ofstream(ad / "Plugins.txt") << "# vanilla\n";
    fs::create_directories(t / "inst/profiles/Default");
    fs::create_symlink(t / "inst/profiles/Default/plugins.txt", ad / "plugins.txt");
    // 缺的 master 其实在 Hotfix 的 Data/ 下
    plugin(t / "inst/mods/Main/Main.esp", 0, {"Skyrim.esm", "Needed.esm"});
    plugin(t / "inst/mods/Hotfix/Data/Needed.esm", 0x1, {"Skyrim.esm"});
    touch(t / "inst/mods/Hotfix/readme.txt");
    std::ofstream(t / "inst/profiles/Default/modlist.txt") << "+Hotfix\n+Main\n";
    std::ofstream(t / "inst/profiles/Default/plugins.txt") << "*Main.esp\n";
    Instance i;
    i.root.assign((t / "inst").string());
    i.profiles_dir.assign((t / "inst/profiles").string());
    i.mods_dir.assign((t / "inst/mods").string());
    i.overwrite_dir.assign((t / "inst/overwrite").string());
    i.farm_path.assign((t / "inst/farm").string());
    i.cfg.game_dir.assign((t / "game").string());
    i.cfg.prefix.assign((t / "pfx").string());
    i.cfg.prefix_user.assign("steamuser");
    i.cfg.profile.assign("Default");
    i.cfg.runner_kind.assign("wine");

    const auto cs = run_doctor(i, "1.6.1170.0");
    CHECK_EQ(level_of(cs, "game.content_catalog"), std::string("error"));
    CHECK_EQ(fix_of(cs, "game.content_catalog"), std::string("fix content-catalog "));
    CHECK_EQ(level_of(cs, "prefix.vcrun"), std::string("error"));
    CHECK_EQ(fix_of(cs, "prefix.vcrun"), std::string("fix vcrun "));
    CHECK_EQ(level_of(cs, "prefix.case_shadows"), std::string("error"));
    CHECK_EQ(level_of(cs, "mods.layout.nested_data"), std::string("warn"));
    CHECK(msg_of(cs, "plugins.masters").find("in mod 'Hotfix' at Data/Needed.esm") != std::string::npos);
    // 1.7.x 读得了那条 Version：不报
    CHECK_EQ(level_of(run_doctor(i, "1.7.104.0"), "game.content_catalog"), std::string("ok"));
    fs::remove_all(t);
}
