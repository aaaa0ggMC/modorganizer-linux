#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstdlib>
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

TEST(start_steam_waits_for_a_fresh_login_line) {
    Tmp t;
    // 假的 steam：启动后过一会儿往 connection_log 追加登录完成行，然后变成一个名为 steam 的常驻进程
    const fs::path bin = t.dir / "bin";
    const fs::path root = t.dir / "steamroot";
    fs::create_directories(bin);
    put(root / "logs/connection_log.txt", "[2026-01-01 00:00:00] [Logged On, 4, 7] old line from a previous session\n");
    fs::create_directories(bin / "d");
    fs::copy_file("/bin/sleep", bin / "d/steam");  // 进程的 exe 名必须是 steam
    put(bin / "steam", "#!/bin/sh\n(sleep 1; echo '[2026-01-01 00:00:01] [Logged On, 4, 7] processing complete' >> '" + (root / "logs/connection_log.txt").string() +
                           "') &\nexec '" + (bin / "d/steam").string() + "' 20\n");
    fs::permissions(bin / "steam", fs::perms::owner_all);
    const std::string old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ::setenv("PATH", (bin.string() + ":" + old_path).c_str(), 1);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = h::start_steam_and_wait(root.string(), 15000);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
    ::setenv("PATH", old_path.c_str(), 1);
    CHECK(ok);
    CHECK(secs >= 1);  // 旧的登录行不算：等到新追加的那一行
    std::system(("pkill -f '" + (bin / "d/steam").string() + "' 2>/dev/null").c_str());
}

TEST(crashlogger_1_24_log_from_the_contentcatalog_crash_is_summarised) {
    // 真实日志（2026-10-07 故意复现：1.6.1170 + 被 1.7 写坏的 ContentCatalog.txt；去掉了硬件信息与 MODULES 之后的部分）
    std::ifstream in(std::string(MOL_TEST_DATA_DIR) + "/crashlogger_1_24_contentcatalog.log", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!text.empty());
    const auto c = h::parse_crash_log(text);
    CHECK(c.exception.find("C++ Exception") != std::string::npos);
    CHECK_EQ(c.cxx_type, std::string("std::invalid_argument*"));
    CHECK_EQ(c.cxx_info, std::string("invalid stoull argument"));
    CHECK_EQ(c.first_own_frame, std::string("SkyrimSE.exe+1235AE9"));
    CHECK(std::find(c.modules.begin(), c.modules.end(), "MSVCP140.dll") != c.modules.end());
    CHECK(std::find(c.modules.begin(), c.modules.end(), "EngineFixes.dll") == c.modules.end());  // [S] 帧不算
    CHECK(std::find(c.files.begin(), c.files.end(), "ContentCatalog.txt") != c.files.end());
    CHECK(std::find(c.files.begin(), c.files.end(), "atalog.txt") == c.files.end());  // 栈上的字符串残片
    CHECK(std::find(c.plugins.begin(), c.plugins.end(), "Constellations - Hard Mode.esp") != c.plugins.end());
    CHECK(c.hint.find("fix content-catalog") != std::string::npos);
}

TEST(proton_log_names_the_throwing_module_and_the_gpu) {
    // 首次排查时的形状：MSVCP140 加载在 0x6FFFFD900000，C++ 异常的 info[3] 指向它；DXVK 选了核显
    const std::string log =
        "012c:trace:loaddll:build_module Loaded L\"C:\\\\windows\\\\system32\\\\MSVCP140.dll\" at 00006FFFFD900000: native\n"
        "012c:trace:loaddll:build_module Loaded L\"C:\\\\windows\\\\system32\\\\kernelbase.dll\" at 00006FFFFFC10000: builtin\n"
        "012c:trace:seh:dispatch_exception code=406d1388 flags=0 addr=00006FFFFFC1CF07 ip=6fffffc1cf07\n"
        "info:  NVIDIA GeForce RTX 5060 Laptop GPU:\n"
        "info:    Driver : NVIDIA 615.71.9\n"
        "info:  AMD Radeon 610M (RADV RAPHAEL_MENDOCINO):\n"
        "info:    Driver : radv 26.2.3\n"
        "info:  Device properties:\n"
        "info:    Device : AMD Radeon 610M (RADV RAPHAEL_MENDOCINO)\n"
        "0294:trace:seh:dispatch_exception code=e06d7363 flags=1 addr=00006FFFFFC1CF07 ip=6fffffc1cf07\n"
        "0294:trace:seh:dispatch_exception  info[0]=0000000019930520\n"
        "0294:trace:seh:dispatch_exception  info[3]=00006ffffd900000\n"
        "013c:trace:seh:dispatch_exception code=6ba flags=0 addr=00006FFFFFC1CF07 ip=6fffffc1cf07\n";
    const auto d = h::parse_proton_log(log);
    CHECK(d.last.has_value());
    CHECK_EQ(d.last->code, std::string("e06d7363"));
    CHECK_EQ(d.last->module, std::string("MSVCP140.dll"));
    CHECK_EQ(d.codes.size(), std::size_t{1});  // 线程命名、RPC 噪音不算
    CHECK_EQ(d.gpus.size(), std::size_t{2});
    CHECK_EQ(d.gpu_used, std::string("AMD Radeon 610M (RADV RAPHAEL_MENDOCINO)"));
    CHECK_EQ(h::discrete_gpu_unused(d), std::string("NVIDIA GeForce RTX 5060 Laptop GPU"));
}
