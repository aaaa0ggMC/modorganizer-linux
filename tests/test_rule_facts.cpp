#include <cstring>

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

// ---- WP-C：影响面事实进入 rules ctx -------------------------------------------
namespace {
// 最小 PE32+（只有节表；无导入导出），用于夹具
std::string mini_pe(bool wx) {
    std::string b(0x400, '\0');
    b[0] = 'M'; b[1] = 'Z';
    std::uint32_t off = 0x40;
    std::memcpy(&b[0x3c], &off, 4);
    b[off] = 'P'; b[off+1] = 'E';
    std::memcpy(&b[off+4], "d", 2);   // amd64
    std::uint16_t nsec = 1, optsz = 0xF0;
    std::memcpy(&b[off+6], &nsec, 2);
    std::memcpy(&b[off+20], &optsz, 2);
    std::uint16_t magic = 0x20b;
    std::memcpy(&b[off+24], &magic, 2);
    std::uint32_t hdrs = 0x200;
    std::memcpy(&b[off+24+60], &hdrs, 4);
    const std::size_t sec = off + 24 + optsz;
    std::memcpy(&b[sec], ".text", 5);
    std::uint32_t vsz = 0x200, va = 0x1000, rsz = 0x200, rptr = 0x200;
    std::memcpy(&b[sec+8], &vsz, 4); std::memcpy(&b[sec+12], &va, 4);
    std::memcpy(&b[sec+16], &rsz, 4); std::memcpy(&b[sec+20], &rptr, 4);
    std::uint32_t ch = wx ? (0x80000000u | 0x20000000u) : 0x60000000u;
    std::memcpy(&b[sec+36], &ch, 4);
    return b;
}
void write_bin(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
} // namespace

// ctx.impact 带上启用 mod 的注入地点；内置规则据此报告 proxy DLL（只告知，不停用）
TEST(impact_facts_feed_the_builtin_rule) {
    Fixture f;
    // 代理 DLL（mod 根）+ 普通 mod
    write_bin(f.root / "mods/ENB Binaries/d3d11.dll", mini_pe(false));
    write(f.root / "mods/Plain Mod/readme.txt", "hi");
    write(f.root / "profiles/Default/modlist.txt", "+ENB Binaries\n+Plain Mod\n");
    // 禁用的不进事实
    fs::create_directories(f.root / "mods/Disabled Mod");
    {   // 追加一行（write() 只截断写）
        std::ofstream out(f.root / "profiles/Default/modlist.txt", std::ios::app);
        out << "-Disabled Mod\n";
    }

    auto ctx = mol::rules::collect_context(f.inst, "1.6.1170");
    CHECK_EQ(ctx.impact.size(), std::size_t{2});   // 两个启用的；禁用的不算
    bool saw_proxy = false, saw_disabled = false;
    for (auto &im : ctx.impact) {
        if (im.mod == "Disabled Mod") saw_disabled = true;
        for (auto &i : im.injections)
            if (i.kind == "proxy_dll" && i.reach == "all-processes") saw_proxy = true;
    }
    CHECK(saw_proxy);
    CHECK(!saw_disabled);

    auto rows = mol::rules::evaluate(mol::rules::builtin_sources(), ctx);
    bool reported = false, has_fix = false;
    for (auto &r : rows)
        if (r.id == "lua.skyrim.injection.proxy_dll") {
            reported = true;
            has_fix = !r.fix.empty();   // 影响 ≠ 责任：永远不给停用动作
        }
    CHECK(reported);
    CHECK(!has_fix);
}

// 加壳 DLL（可写+可执行节）→ packed_suspect + warn
TEST(packed_dll_is_flagged_and_warned) {
    Fixture f;
    write_bin(f.root / "mods/Nemesis/Nemesis.dll", mini_pe(true));
    write(f.root / "profiles/Default/modlist.txt", "+Nemesis\n");
    auto ctx = mol::rules::collect_context(f.inst, "1.6.1170");
    bool packed = false;
    for (auto &im : ctx.impact)
        if (im.mod == "Nemesis") packed = im.packed_suspect;
    CHECK(packed);
    auto rows = mol::rules::evaluate(mol::rules::builtin_sources(), ctx);
    bool warned = false;
    for (auto &r : rows)
        if (r.id == "lua.skyrim.injection.packed" && r.level == "warn") warned = true;
    CHECK(warned);
}
