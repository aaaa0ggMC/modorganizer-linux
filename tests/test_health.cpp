#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <map>

#include "minitest.hpp"
#include "mol/health.hpp"
#include "mol/instance.hpp"

namespace fs = std::filesystem;
using namespace mol;
namespace h = mol::health;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_health_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void put(const fs::path& p, const std::string& body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}
Instance make(const Tmp& t) {
    Instance i;
    i.root.assign((t.dir / "inst").string());
    i.mods_dir.assign((t.dir / "inst/mods").string());
    i.profiles_dir.assign((t.dir / "inst/profiles").string());
    i.overwrite_dir.assign((t.dir / "inst/overwrite").string());
    i.cfg.game_dir.assign((t.dir / "game").string());
    i.cfg.prefix.assign((t.dir / "pfx").string());
    i.cfg.prefix_user.assign("steamuser");
    i.cfg.profile.assign("Default");
    fs::create_directories(t.dir / "inst/profiles/Default");
    fs::create_directories(t.dir / "inst/mods");
    fs::create_directories(t.dir / "game/Data");
    return i;
}
// 最小的「PE」：MZ 头 + 一段带 VS_FIXEDFILEINFO 的字节
std::string fake_pe(int a, int b, int c, int d) {
    std::string s = "MZ";
    s.resize(200, '\0');
    auto le = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); };
    le(0xFEEF04BD);
    le(0x00010000);
    le(static_cast<std::uint32_t>(a) << 16 | static_cast<std::uint32_t>(b));
    le(static_cast<std::uint32_t>(c) << 16 | static_cast<std::uint32_t>(d));
    s.resize(s.size() + 40, '\0');
    return s;
}
}  // namespace

TEST(content_catalog_versions_written_by_1_7_are_flagged) {
    // 真实文件里的样子（1.7.104 写的，1.6.1170 读到第二条就抛异常）
    const std::string text = R"({
  "CSV2_016105c0" : { "Files" : [ "a.esm" ], "Version" : "1663174652.4" },
  "CSV2_0270ab24" : { "Files" : [ "b.esl" ], "Version" : "1701307962.== Version Number ==" },
  "CSV2_x" : { "Version":"17" },
  "CSV2_y" : { "Version" : "1701307962.== Version Number ==" }
})";
    const auto bad = h::bad_catalog_versions(text);
    CHECK_EQ(bad.size(), std::size_t{1});
    CHECK_EQ(bad[0], std::string("1701307962.== Version Number =="));
    CHECK(h::bad_catalog_versions(R"({"a":{"Version" : "1.2"}})").empty());
    CHECK(h::catalog_versions_harmful("1.6.1170.0"));
    CHECK(h::catalog_versions_harmful("1.5.97.0"));
    CHECK(h::catalog_versions_harmful(""));
    CHECK(!h::catalog_versions_harmful("1.7.104.0"));
}

TEST(content_catalog_is_moved_aside_not_deleted) {
    Tmp t;
    const Instance inst = make(t);
    CHECK(h::move_content_catalog_aside(inst).empty());  // 没有就什么都不做
    put(h::appdata_dir(inst) / "ContentCatalog.txt", "{}");
    const std::string bak = h::move_content_catalog_aside(inst);
    CHECK(!bak.empty());
    CHECK(fs::exists(bak));
    CHECK(!fs::exists(h::appdata_dir(inst) / "ContentCatalog.txt"));
}

TEST(vc_runtime_versions_are_read_from_the_pe_resources) {
    Tmp t;
    const Instance inst = make(t);
    const fs::path sys = t.dir / "pfx/drive_c/windows/system32";
    put(sys / "msvcp140.dll", fake_pe(14, 0, 24215, 1));
    put(sys / "vcruntime140.dll", fake_pe(14, 44, 35211, 0));
    auto rt = h::vc_runtime(inst);
    CHECK_EQ(rt.size(), std::size_t{3});
    CHECK_EQ(rt[0].version, std::string("14.0.24215.1"));
    CHECK(!rt[0].ok);
    CHECK(rt[1].ok);
    CHECK(rt[2].version.empty());  // vcruntime140_1.dll 不存在
    CHECK(!h::vc_runtime_ok(rt));
    put(sys / "msvcp140.dll", fake_pe(14, 40, 33810, 0));
    put(sys / "VCRUNTIME140_1.DLL", fake_pe(14, 44, 35211, 0));  // 大小写不敏感
    CHECK(h::vc_runtime_ok(h::vc_runtime(inst)));
    put(sys / "msvcp140.dll", "MZ not really a dll");
    CHECK(!h::pe_file_version(sys / "msvcp140.dll"));
}

TEST(case_shadow_of_plugins_txt_is_reported_only_next_to_our_link) {
    Tmp t;
    const Instance inst = make(t);
    const fs::path ad = h::appdata_dir(inst);
    put(ad / "Plugins.txt", "# vanilla");
    CHECK(h::case_shadows(inst).empty());  // 还没 sync：只有原版文件，不算影子
    fs::create_symlink(t.dir / "inst/profiles/Default/plugins.txt", ad / "plugins.txt");
    const auto sh = h::case_shadows(inst);
    CHECK_EQ(sh.size(), std::size_t{1});
    CHECK_EQ(fs::path(sh[0]).filename().string(), std::string("Plugins.txt"));
}

TEST(skse_log_failures_and_successes_are_counted) {
    const auto log = h::parse_skse_log(
        "checking plugin CrashLogger.dll\r\n"
        "plugin CrashLogger.dll (00000001 CrashLogger 01180000) disabled, fatal error occurred while loading plugin 0 (handle 35)\r\n"
        "couldn't load plugin D:\\Constellations\\farm\\Data\\SKSE\\Plugins\\hdtsmp64.dll (000003E6)\n"
        "plugin TrueHUD.dll (00000001 TrueHUD 01010090) reported as incompatible during load 0 (handle 187)\n"
        "plugin po3_Tweaks.dll (00000001 powerofthree's Tweaks 01080000) loaded correctly (handle 7)\n");
    CHECK_EQ(log.loaded, std::size_t{1});
    CHECK_EQ(log.failed.size(), std::size_t{2});
    CHECK_EQ(log.failed[0], std::string("CrashLogger.dll"));
    CHECK_EQ(log.failed[1], std::string("hdtsmp64.dll"));
    CHECK_EQ(log.incompatible.size(), std::size_t{1});
    CHECK_EQ(log.incompatible[0], std::string("TrueHUD.dll"));
}

TEST(enb_is_wanted_by_presets_or_helpers_and_satisfied_by_d3d11) {
    Tmp t;
    const Instance inst = make(t);
    put(t.dir / "inst/mods/Helper/KiLoader/Plugins/ENBHelperSE.dll");
    put(t.dir / "inst/profiles/Default/modlist.txt", "+Helper\n");
    CHECK(h::enb_wanted_by(inst).find("ENBHelperSE.dll") != std::string::npos);
    CHECK(!h::enb_binaries_present(inst));
    put(t.dir / "inst/mods/ENB Binaries/d3d11.dll");
    put(t.dir / "inst/mods/ENB Binaries/meta.ini", "[General]\nmol_root=true\n");
    put(t.dir / "inst/profiles/Default/modlist.txt", "+ENB Binaries\n+Helper\n");
    CHECK(h::enb_binaries_present(inst));
    // 禁用后不算
    put(t.dir / "inst/profiles/Default/modlist.txt", "-ENB Binaries\n+Helper\n");
    CHECK(!h::enb_binaries_present(inst));
}

TEST(mod_layouts_broken_by_old_installs_are_found) {
    Tmp t;
    const Instance inst = make(t);
    put(t.dir / "inst/mods/Hotfix/Data/3DNPC.esp");
    put(t.dir / "inst/mods/Hotfix/Patch Notes.txt");
    put(t.dir / "inst/mods/Hotfix/meta.ini", "[General]\nmodid=1\n");     // meta.ini 不能让它看起来像 Data 根
    put(t.dir / "inst/mods/Hub/ModuleConfig.xml");
    put(t.dir / "inst/mods/Priority/Data\\SKSE\\Plugins\\PriorityMod.dll");
    put(t.dir / "inst/mods/Preset/enbseries/a.fx");
    put(t.dir / "inst/mods/Fine/Data/x.txt");
    put(t.dir / "inst/mods/Fine/meshes/m.nif");                          // 顶层已是游戏数据：Data/ 是它自己的
    put(t.dir / "inst/mods/Root/Data/SKSE/Plugins/x.dll");
    put(t.dir / "inst/mods/Root/enbseries/b.fx");
    put(t.dir / "inst/mods/Root/meta.ini", "[General]\nmol_root=true\n");  // 根目录型：Data/ 与 enbseries 都正确
    put(t.dir / "inst/profiles/Default/modlist.txt", "+Hotfix\n+Hub\n+Priority\n+Preset\n+Fine\n+Root\n");
    std::map<std::string, std::string> got;
    for (const auto& i : h::scan_mod_layouts(inst)) got[i.mod] += i.kind + ";";
    CHECK_EQ(got["Hotfix"], std::string("nested_data;"));
    CHECK_EQ(got["Hub"], std::string("raw_fomod;"));
    CHECK_EQ(got["Priority"], std::string("backslash_name;"));
    CHECK_EQ(got["Preset"], std::string("enb_in_data;"));
    CHECK(got.count("Fine") == 0);
    CHECK(got.count("Root") == 0);
}
