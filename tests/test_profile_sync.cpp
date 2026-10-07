#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/plugins_sync.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_ps_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void put(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
Instance make(const Tmp& t) {
    Instance i;
    i.root.assign((t.dir / "inst").string());
    i.profiles_dir.assign((t.dir / "inst/profiles").string());
    i.cfg.profile.assign("P");
    i.cfg.prefix.assign((t.dir / "pfx").string());
    i.cfg.prefix_user.assign("steamuser");
    fs::create_directories(t.dir / "inst/profiles/P");
    return i;
}
std::string actions(const SyncReport& r) {
    std::string s;
    for (const auto& e : r.entries) { s += std::string(e.action); s += ";"; }
    return s;
}
}  // namespace

TEST(local_settings_link_inis_and_write_through) {
    Tmp t;
    const Instance inst = make(t);
    const fs::path docs = t.dir / "pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition";
    put(t.dir / "inst/profiles/P/settings.ini", "[General]\nLocalSettings=true\nLocalSaves=false\n");
    put(t.dir / "inst/profiles/P/skyrim.ini", "[General]\nsLanguage=ENGLISH\n");
    put(t.dir / "inst/profiles/P/SKYRIMPREFS.INI", "[Display]\n");
    put(docs / "Skyrim.ini", "[General]\nsLanguage=FRENCH\n");  // 游戏自己建的（大小写不同于 profile）

    SyncReport r;
    sync_profile_settings(inst, r);
    CHECK(r.changed);
    CHECK_EQ(actions(r), std::string("backup+link;link;"));
    CHECK(fs::is_symlink(docs / "Skyrim.ini"));                       // 沿用前缀里已有的名字写法
    CHECK_EQ(slurp(docs / "Skyrim.ini"), std::string("[General]\nsLanguage=ENGLISH\n"));
    CHECK_EQ(slurp(docs / "Skyrim.ini.mol-backup"), std::string("[General]\nsLanguage=FRENCH\n"));  // 原文件被保留
    CHECK(fs::is_symlink(docs / "SkyrimPrefs.ini"));                  // 前缀里没有 → 用标准名字
    // 游戏写它 = 写进 profile
    { std::ofstream(docs / "SkyrimPrefs.ini", std::ios::app) << "iSize=1\n"; }
    CHECK_EQ(slurp(t.dir / "inst/profiles/P/SKYRIMPREFS.INI"), std::string("[Display]\niSize=1\n"));
    // 幂等
    SyncReport r2;
    sync_profile_settings(inst, r2);
    CHECK(!r2.changed);
    CHECK_EQ(actions(r2), std::string("ok;ok;"));
}

TEST(settings_off_means_nothing_is_touched) {
    Tmp t;
    const Instance inst = make(t);
    put(t.dir / "inst/profiles/P/skyrim.ini", "x");
    put(t.dir / "inst/profiles/P/settings.ini", "[General]\nLocalSettings=false\n");
    SyncReport r;
    sync_profile_settings(inst, r);
    CHECK(!r.changed);
    CHECK(r.entries.empty());
}

TEST(local_saves_link_but_never_clobber_real_saves) {
    Tmp t;
    const Instance inst = make(t);
    const fs::path docs = t.dir / "pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition";
    put(t.dir / "inst/profiles/P/settings.ini", "[General]\nLocalSaves=true\n");
    put(t.dir / "inst/profiles/P/saves/a.ess", "SAVE");
    put(docs / "Saves/real.ess", "PRECIOUS");  // 用户真实存档目录，有内容
    SyncReport r;
    sync_profile_settings(inst, r);
    CHECK_EQ(actions(r), std::string("skip-existing-directory;"));
    CHECK(!fs::is_symlink(docs / "Saves"));
    CHECK_EQ(slurp(docs / "Saves/real.ess"), std::string("PRECIOUS"));
    // 目标是空目录（游戏刚建的）→ 可以换成链接
    fs::remove(docs / "Saves/real.ess");
    SyncReport r2;
    sync_profile_settings(inst, r2);
    CHECK_EQ(actions(r2), std::string("link;"));
    CHECK(fs::is_symlink(docs / "Saves"));
    CHECK_EQ(slurp(docs / "Saves/a.ess"), std::string("SAVE"));
}

TEST(case_variant_shadows_of_the_target_are_backed_up) {
    Tmp t;
    // 游戏原版运行时建的 Plugins.txt；映射目标是 plugins.txt。Wine 会优先打开大小写一致的 Plugins.txt。
    put(t.dir / "Plugins.txt", "# vanilla\n");
    fs::create_symlink(t.dir / "elsewhere", t.dir / "PLUGINS.TXT");
    put(t.dir / "loadorder.txt", "keep\n");
    CHECK_EQ(backup_case_variants(t.dir / "plugins.txt"), std::size_t{2});
    CHECK(!fs::exists(fs::symlink_status(t.dir / "Plugins.txt")));
    CHECK(!fs::exists(fs::symlink_status(t.dir / "PLUGINS.TXT")));
    CHECK(fs::exists(t.dir / "Plugins.txt.mol-backup"));
    CHECK(fs::exists(t.dir / "loadorder.txt"));
    CHECK_EQ(backup_case_variants(t.dir / "plugins.txt"), std::size_t{0});  // 幂等
    // 已有备份 → 拒绝，不覆盖
    put(t.dir / "Plugins.txt", "# again\n");
    bool threw = false;
    try { backup_case_variants(t.dir / "plugins.txt"); } catch (const Error&) { threw = true; }
    CHECK(threw);
    CHECK(fs::exists(t.dir / "Plugins.txt"));
}
