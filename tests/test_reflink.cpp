#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/reflink.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
fs::path base_dir() {
    // 要真测 reflink 得在 btrfs/xfs 上：MOL_TEST_REFLINK_DIR=/某个 btrfs 目录；没给就用临时目录（多半是 tmpfs/ext4，测回退）
    if (const char* d = std::getenv("MOL_TEST_REFLINK_DIR"); d && *d) return fs::path(d) / ("mol_reflink_" + std::to_string(::getpid()));
    return fs::temp_directory_path() / ("mol_reflink_" + std::to_string(::getpid()));
}
void put(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
}  // namespace

TEST(reflink_tree_copies_or_cleanly_refuses) {
    const fs::path t = base_dir();
    fs::remove_all(t);
    put(t / "src/meshes/a.nif", std::string(100000, 'A'));
    put(t / "src/meta.ini", "[General]\nmodid=1\n");
    const bool ok = reflink_tree(t / "src", t / "dst");
    if (ok) {
        CHECK_EQ(slurp(t / "dst/meshes/a.nif"), std::string(100000, 'A'));
        CHECK_EQ(slurp(t / "dst/meta.ini"), slurp(t / "src/meta.ini"));
        put(t / "dst/meshes/a.nif", "changed");  // 独立副本：改它不影响源
        CHECK_EQ(slurp(t / "src/meshes/a.nif"), std::string(100000, 'A'));
    } else {
        CHECK(!fs::exists(t / "dst"));  // 不支持 reflink：不留半成品
    }
    CHECK(!reflink_tree(t / "src", t / "src"));  // 目标已存在 → 拒绝
    fs::remove_all(t);
}

TEST(dedupe_shares_only_identical_files) {
    const fs::path t = base_dir();
    fs::remove_all(t);
    const std::string big(300000, 'Z');
    put(t / "a/textures/same.dds", big);
    put(t / "a/textures/diff.dds", std::string(300000, 'X'));
    put(t / "b/Textures/same.dds", big);                     // 大小写不同的路径也认
    put(t / "b/textures/diff.dds", std::string(300000, 'Y'));  // 同大小、内容不同：不能动
    put(t / "b/only_here.esp", "E");
    const DedupeStats ds = dedupe_tree(t / "b", t / "a");
    CHECK(ds.files <= 1);  // 文件系统不支持时为 0
    CHECK_EQ(slurp(t / "b/textures/diff.dds"), std::string(300000, 'Y'));
    CHECK_EQ(slurp(t / "b/Textures/same.dds"), big);
    if (std::getenv("MOL_TEST_REFLINK_DIR")) CHECK_EQ(ds.files, std::uint64_t{1});
    fs::remove_all(t);
}
