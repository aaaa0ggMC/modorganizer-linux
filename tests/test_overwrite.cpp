#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "minitest.hpp"
#include "mol/overwrite.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_ow_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void put(const fs::path& p, const char* body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
Instance make(const Tmp& t) {
    Instance i;
    i.root.assign((t.dir / "inst").string());
    i.overwrite_dir.assign((t.dir / "inst/overwrite").string());
    i.farm_path.assign((t.dir / "inst/farm").string());
    fs::create_directories(t.dir / "inst/farm");
    fs::create_directories(t.dir / "inst/overwrite");
    return i;
}
}  // namespace

TEST(capture_moves_real_files_and_keeps_links) {
    Tmp t;
    const Instance i = make(t);
    put(t.dir / "src/linked.txt", "L");
    fs::create_directories(t.dir / "inst/farm/Data/Meshes");
    fs::create_symlink(t.dir / "src/linked.txt", t.dir / "inst/farm/Data/linked.txt");
    put(t.dir / "inst/farm/.mol-farm.json", "{}");
    put(t.dir / "inst/farm/Data/Meshes/new.nif", "N");   // 游戏新建
    put(t.dir / "inst/farm/data/Other/x.esp", "E");      // 大小写不同的 Data
    put(t.dir / "inst/farm/crash.log", "C");             // Data 之外
    CHECK_EQ(capture_overwrite(i), std::size_t{3});
    CHECK_EQ(slurp(t.dir / "inst/overwrite/Meshes/new.nif"), std::string("N"));
    CHECK_EQ(slurp(t.dir / "inst/overwrite/Other/x.esp"), std::string("E"));
    CHECK_EQ(slurp(t.dir / "inst/overwrite-root/crash.log"), std::string("C"));
    CHECK(fs::is_symlink(t.dir / "inst/farm/Data/linked.txt"));
    CHECK(fs::exists(t.dir / "inst/farm/.mol-farm.json"));
    CHECK(!fs::exists(t.dir / "inst/farm/Data/Meshes"));  // 变空的目录被清理
    CHECK_EQ(capture_overwrite(i), std::size_t{0});        // 幂等
}

TEST(capture_backs_up_instead_of_clobbering) {
    Tmp t;
    const Instance i = make(t);
    put(t.dir / "inst/overwrite/a.txt", "OLD");
    put(t.dir / "inst/farm/Data/a.txt", "NEW");
    CHECK_EQ(capture_overwrite(i), std::size_t{1});
    CHECK_EQ(slurp(t.dir / "inst/overwrite/a.txt"), std::string("NEW"));
    CHECK_EQ(slurp(t.dir / "inst/overwrite-backup/overwrite/a.txt"), std::string("OLD"));
}

TEST(farm_in_use_detects_process_with_cwd_in_farm) {
    Tmp t;
    const Instance i = make(t);
    CHECK(!farm_in_use(i));
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (::chdir((t.dir / "inst/farm").c_str()) != 0) _exit(1);
        ::execlp("sleep", "sleep", "30", static_cast<char*>(nullptr));
        _exit(127);
    }
    bool busy = false;
    for (int n = 0; n < 50 && !busy; ++n) {
        ::usleep(20000);
        busy = farm_in_use(i);
    }
    CHECK(busy);
    bool threw = false;
    try { require_farm_idle(i); } catch (const Error& e) { threw = (e.code == "farm_busy"); }
    CHECK(threw);
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    CHECK(!farm_in_use(i));
}

TEST(promote_previews_then_moves_without_clobbering) {
    Tmp t;
    Instance i = make(t);
    i.cfg.game_dir.assign((t.dir / "game").string());
    put(t.dir / "game/DATA/Skyrim.esm", "G");  // 游戏目录里的目录名是大写
    put(t.dir / "inst/overwrite/ccA.esl", "A");
    put(t.dir / "inst/overwrite/ccA.bsa", "B");
    put(t.dir / "inst/overwrite/Meshes/m.nif", "M");
    put(t.dir / "inst/overwrite/Skyrim.esm", "FAKE");  // 目标已存在
    const std::vector<std::string> f{"cc*", "skyrim.esm"};

    auto prev = promote_overwrite(i, f, false);
    CHECK_EQ(prev.size(), std::size_t{3});
    CHECK(fs::exists(t.dir / "inst/overwrite/ccA.esl"));  // 预览不动文件
    CHECK(!fs::exists(t.dir / "game/DATA/ccA.esl"));

    auto done = promote_overwrite(i, f, true);
    CHECK_EQ(done.size(), std::size_t{3});
    CHECK_EQ(slurp(t.dir / "game/DATA/ccA.esl"), std::string("A"));  // 沿用 DATA 的写法
    CHECK_EQ(slurp(t.dir / "game/DATA/Skyrim.esm"), std::string("G"));  // 不覆盖
    CHECK(fs::exists(t.dir / "inst/overwrite/Skyrim.esm"));
    CHECK(fs::exists(t.dir / "inst/overwrite/Meshes/m.nif"));  // 未匹配的不动
    CHECK(promote_overwrite(i, {}, true).empty());
}

namespace {
// 一个最小的「真」农场：manifest 里登记了 rel 下的这些链接
void make_farm(const Tmp& t, const std::vector<std::pair<std::string, fs::path>>& links) {
    std::string created;
    for (const auto& [rel, target] : links) {
        fs::create_directories((t.dir / "inst/farm" / rel).parent_path());
        fs::create_symlink(target, t.dir / "inst/farm" / rel);
        created += (created.empty() ? "\"" : ",\"") + rel + "\"";
    }
    put(t.dir / "inst/farm/.mol-farm.json", ("{\"version\":1,\"created\":[" + created + "]}").c_str());
}
}  // namespace

TEST(capture_drops_unchanged_cow_copies_and_keeps_changed_ones) {
    Tmp t;
    const Instance i = make(t);
    put(t.dir / "game/Data/same.esp", "S");
    put(t.dir / "game/Data/edited.ini", "old");
    make_farm(t, {{"Data/same.esp", t.dir / "game/Data/same.esp"}, {"Data/edited.ini", t.dir / "game/Data/edited.ini"}});
    // libmol-cow 已把两个链接换成副本：一个没改（只是以写方式打开），一个改了
    for (const char* f : {"same.esp", "edited.ini"}) fs::remove(t.dir / "inst/farm/Data" / f);
    put(t.dir / "inst/farm/Data/same.esp", "S");
    put(t.dir / "inst/farm/Data/edited.ini", "new");
    put(t.dir / "inst/.mol-cow.log", ("Data/same.esp\t" + (t.dir / "game/Data/same.esp").string() + "\treflink\nData/edited.ini\t" +
                                      (t.dir / "game/Data/edited.ini").string() + "\tcopy\n").c_str());
    put(t.dir / "inst/farm/.mol-cow.lock", "");
    const auto st = cow_stats(i);
    CHECK_EQ(st.copies, std::size_t{2});
    CHECK_EQ(st.reflinked, std::size_t{1});
    CHECK_EQ(capture_overwrite(i), std::size_t{1});
    CHECK(!fs::exists(t.dir / "inst/overwrite/same.esp"));          // 没改：丢弃，apply 会恢复链接
    CHECK(!fs::exists(fs::symlink_status(t.dir / "inst/farm/Data/same.esp")));
    CHECK_EQ(slurp(t.dir / "inst/overwrite/edited.ini"), std::string("new"));
    CHECK_EQ(slurp(t.dir / "game/Data/edited.ini"), std::string("old"));  // 原文件不动
    CHECK(!fs::exists(t.dir / "inst/.mol-cow.log"));
    CHECK(!fs::exists(t.dir / "inst/farm/.mol-cow.lock"));          // 锁不会被当成工具产物收走
    CHECK(!fs::exists(t.dir / "inst/overwrite-root/.mol-cow.lock"));
}

TEST(capture_materializes_links_a_tool_moved_and_root_files_go_to_overwrite_root) {
    Tmp t;
    const Instance i = make(t);
    put(t.dir / "game/SkyrimSE.exe", "v17");
    make_farm(t, {{"SkyrimSE.exe", t.dir / "game/SkyrimSE.exe"}});
    // 工具把链接挪进备份目录（原生 rename 挪的是链接本身），再写一个新的 exe
    fs::create_directories(t.dir / "inst/farm/bak");
    fs::rename(t.dir / "inst/farm/SkyrimSE.exe", t.dir / "inst/farm/bak/SkyrimSE.exe");
    put(t.dir / "inst/farm/SkyrimSE.exe", "v16");
    CHECK_EQ(capture_overwrite(i), std::size_t{2});
    CHECK_EQ(slurp(t.dir / "inst/overwrite-root/SkyrimSE.exe"), std::string("v16"));
    CHECK(fs::is_regular_file(fs::symlink_status(t.dir / "inst/overwrite-root/bak/SkyrimSE.exe")));  // 真备份，不是链接
    CHECK_EQ(slurp(t.dir / "inst/overwrite-root/bak/SkyrimSE.exe"), std::string("v17"));
    CHECK_EQ(slurp(t.dir / "game/SkyrimSE.exe"), std::string("v17"));
}

TEST(capture_without_a_readable_manifest_never_touches_links) {
    Tmp t;
    const Instance i = make(t);
    put(t.dir / "game/a.txt", "A");
    fs::create_symlink(t.dir / "game/a.txt", t.dir / "inst/farm/a.txt");
    put(t.dir / "inst/farm/.mol-farm.json", "{ broken");
    CHECK_EQ(capture_overwrite(i), std::size_t{0});
    CHECK(fs::is_symlink(fs::symlink_status(t.dir / "inst/farm/a.txt")));
}

TEST(clone_or_copy_file_copies_content_and_mode) {
    Tmp t;
    put(t.dir / "a.bin", "payload");
    fs::permissions(t.dir / "a.bin", fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
    CHECK(clone_or_copy_file((t.dir / "a.bin").string(), (t.dir / "b.bin").string()));
    CHECK_EQ(slurp(t.dir / "b.bin"), std::string("payload"));
    CHECK((fs::status(t.dir / "b.bin").permissions() & fs::perms::owner_exec) != fs::perms::none);
    CHECK(!clone_or_copy_file((t.dir / "missing").string(), (t.dir / "c.bin").string()));
}
