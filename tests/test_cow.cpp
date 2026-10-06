// libmol-cow.so 的行为测试：用 sh + coreutils 模拟 Wine 的文件操作（不需要 Wine）。
// Wine 的特点是：删除/改名时用的是链接**解析后**的真实路径——这里用 `readlink -f` 复现。
// 不变量：游戏目录与 mod 里的原文件一个字节都不能变。
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "minitest.hpp"

namespace fs = std::filesystem;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_cow_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / "game/Data");
        fs::create_directories(dir / "mods/M");
        fs::create_directories(dir / "farm/Data");
        fs::create_directories(dir / "inst");
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void put(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
bool is_link(const fs::path& p) { return fs::is_symlink(fs::symlink_status(p)); }
bool exists_any(const fs::path& p) { return fs::exists(fs::symlink_status(p)); }

// 在农场根下、注入 COW 后执行 sh 命令
int sh(const Tmp& t, const std::string& cmd) {
    const std::string d = t.dir.string();
    const std::string full = "cd '" + d + "/farm' && env LD_PRELOAD='" MOL_COW_LIB_PATH "' MOL_COW_ROOT='" + d + "/farm' MOL_COW_LOG='" + d +
                             "/inst/cow.log' MOL_COW_PROTECT='" + d + "/game' MOL_COW_MODS='" + d + "/mods' sh -c '" + cmd + "' 2>/dev/null";
    return std::system(full.c_str());
}

void setup(const Tmp& t) {
    put(t.dir / "game/SkyrimSE.exe", "v17");
    put(t.dir / "game/Data/Skyrim.esm", "esm");
    put(t.dir / "mods/M/m.ini", "mod");
    put(t.dir / "mods/M/keep.txt", "keep");
    fs::create_symlink(t.dir / "game/SkyrimSE.exe", t.dir / "farm/SkyrimSE.exe");
    fs::create_symlink(t.dir / "game/Data/Skyrim.esm", t.dir / "farm/Data/Skyrim.esm");
    fs::create_symlink(t.dir / "mods/M/m.ini", t.dir / "farm/Data/m.ini");
    fs::create_symlink(t.dir / "mods/M/keep.txt", t.dir / "farm/Data/Keep.txt");  // 农场里的大小写与 mod 里不同
}
void originals_intact(const Tmp& t) {
    CHECK_EQ(slurp(t.dir / "game/SkyrimSE.exe"), std::string("v17"));
    CHECK_EQ(slurp(t.dir / "game/Data/Skyrim.esm"), std::string("esm"));
    CHECK_EQ(slurp(t.dir / "mods/M/m.ini"), std::string("mod"));
    CHECK_EQ(slurp(t.dir / "mods/M/keep.txt"), std::string("keep"));
}
}  // namespace

TEST(writes_go_to_a_copy_and_reads_keep_the_link) {
    Tmp t;
    setup(t);
    CHECK_EQ(sh(t, "echo tweaked >> Data/m.ini; cat Data/Skyrim.esm > /dev/null; : > SkyrimSE.exe; truncate -s 1 Data/Keep.txt"), 0);
    originals_intact(t);
    CHECK_EQ(slurp(t.dir / "farm/Data/m.ini"), std::string("modtweaked\n"));
    CHECK(!is_link(t.dir / "farm/Data/m.ini"));
    CHECK(is_link(t.dir / "farm/Data/Skyrim.esm"));  // 只读：不复制
    CHECK_EQ(slurp(t.dir / "farm/SkyrimSE.exe"), std::string(""));
    CHECK_EQ(slurp(t.dir / "farm/Data/Keep.txt"), std::string("k"));
    const std::string log = slurp(t.dir / "inst/cow.log");
    CHECK(log.find("Data/m.ini\t" + (t.dir / "mods/M/m.ini").string() + "\t") != std::string::npos);
    CHECK(log.find("SkyrimSE.exe\t") != std::string::npos);
}

TEST(deleting_the_resolved_original_only_removes_the_farm_link) {
    Tmp t;
    setup(t);
    // Wine 的 DeleteFile：unlink(链接目标)。大小写不同的农场名也要找得到
    CHECK_EQ(sh(t, "rm -f \"$(readlink -f Data/m.ini)\" \"$(readlink -f Data/Keep.txt)\""), 0);
    originals_intact(t);
    CHECK(!exists_any(t.dir / "farm/Data/m.ini"));
    CHECK(!exists_any(t.dir / "farm/Data/Keep.txt"));
    CHECK(is_link(t.dir / "farm/Data/Skyrim.esm"));
}

TEST(moving_the_resolved_original_copies_it_and_removes_the_link) {
    Tmp t;
    setup(t);
    // 降级补丁的做法：把 exe 挪进备份目录（Wine 会 rename 链接目标），再写一个新的
    CHECK_EQ(sh(t, "mkdir bak && mv \"$(readlink -f SkyrimSE.exe)\" bak/SkyrimSE.exe && echo v16 > SkyrimSE.exe"), 0);
    originals_intact(t);
    CHECK(!is_link(t.dir / "farm/bak/SkyrimSE.exe"));
    CHECK_EQ(slurp(t.dir / "farm/bak/SkyrimSE.exe"), std::string("v17"));
    CHECK(!is_link(t.dir / "farm/SkyrimSE.exe"));
    CHECK_EQ(slurp(t.dir / "farm/SkyrimSE.exe"), std::string("v16\n"));
}

TEST(writing_or_renaming_into_protected_directories_is_refused) {
    Tmp t;
    setup(t);
    // 直接往原目录里新建/改名进去：拒绝
    CHECK(sh(t, "echo x > \"" + (t.dir / "mods/M/new.txt").string() + "\"") != 0);
    put(t.dir / "farm/tmp.txt", "t");
    CHECK(sh(t, "mv tmp.txt \"" + (t.dir / "game/Data/tmp.txt").string() + "\"") != 0);
    CHECK(!exists_any(t.dir / "mods/M/new.txt"));
    CHECK(!exists_any(t.dir / "game/Data/tmp.txt"));
    // 用解析后的路径写原文件：改成写农场副本
    CHECK_EQ(sh(t, "echo hi >> \"$(readlink -f Data/m.ini)\""), 0);
    originals_intact(t);
    CHECK_EQ(slurp(t.dir / "farm/Data/m.ini"), std::string("modhi\n"));
    // 权限：原文件不改
    CHECK_EQ(sh(t, "chmod 0400 \"$(readlink -f Data/Skyrim.esm)\""), 0);
    CHECK((fs::status(t.dir / "game/Data/Skyrim.esm").permissions() & fs::perms::owner_write) != fs::perms::none);
}

TEST(without_mol_cow_root_the_library_does_nothing) {
    Tmp t;
    setup(t);
    const std::string d = t.dir.string();
    const std::string cmd = "cd '" + d + "/farm' && env LD_PRELOAD='" MOL_COW_LIB_PATH "' sh -c 'echo x >> Data/m.ini' 2>/dev/null";
    CHECK_EQ(std::system(cmd.c_str()), 0);
    CHECK_EQ(slurp(t.dir / "mods/M/m.ini"), std::string("modx\n"));  // 没启用：照常写穿（对照组）
}
