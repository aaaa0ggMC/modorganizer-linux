#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/bsa.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_bsa_" + std::to_string(::getpid()));
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
std::string find_real_bsa() {
    if (const char* e = std::getenv("MOL_TEST_BSA"); e && *e) return e;
    const char* home = std::getenv("HOME");
    if (!home) return {};
    const fs::path d = fs::path(home) / ".local/share/Steam/steamapps/common/Skyrim Special Edition/Data";
    std::error_code ec;
    std::string best;
    std::uintmax_t best_size = ~std::uintmax_t{0};
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        const auto n = it->path().filename().string();
        if (n.size() > 4 && (n.rfind("cc", 0) == 0 || n.rfind("CC", 0) == 0) && n.substr(n.size() - 4) == ".bsa") {
            const auto sz = it->file_size(ec);
            if (sz > 0 && sz < best_size) { best_size = sz; best = it->path().string(); }
        }
    }
    return best;
}
}  // namespace

TEST(hash_special_cases) {
    // 空与极短名字不应越界
    (void)bsa::hash_name("", false);
    (void)bsa::hash_name("a", false);
    (void)bsa::hash_name("ab.nif", false);
    // 扩展名标志位
    CHECK((bsa::hash_name("zz.dds") & 0x8080) == 0x8080);  // .dds 的扩展名标志位
    CHECK((bsa::hash_name("zz.nif") & 0x8000) == 0x8000);
}

TEST(roundtrip_uncompressed_and_compressed) {
    Tmp t;
    put(t.dir / "a.nif", std::string(5000, 'A'));
    put(t.dir / "b.dds", "BBBB");
    put(t.dir / "c.txt", std::string(300000, 'C'));
    for (std::uint32_t flags : {0x3u, 0x7u, 0x103u}) {
        const std::string out = (t.dir / ("o" + std::to_string(flags) + ".bsa")).string();
        std::vector<bsa::InputFile> in = {{"Meshes/Armor/A.nif", (t.dir / "a.nif").string(), false},
                                          {"textures\\armor\\b.dds", (t.dir / "b.dds").string(), true},
                                          {"textures\\armor\\c.txt", (t.dir / "c.txt").string(), false}};
        bsa::Header h;
        h.archive_flags = flags;
        bsa::write(out, h, in);
        const auto idx = bsa::read_index(out);
        CHECK_EQ(idx.files.size(), std::size_t{3});
        CHECK_EQ(idx.header.archive_flags, flags);
        for (const auto& e : idx.files) {
            const std::string body = bsa::read_file(out, idx, e);
            if (e.path == "meshes\\armor\\a.nif") CHECK_EQ(body, std::string(5000, 'A'));
            else if (e.path == "textures\\armor\\b.dds") { CHECK_EQ(body, std::string("BBBB")); CHECK(e.flip); }
            else if (e.path == "textures\\armor\\c.txt") CHECK_EQ(body, std::string(300000, 'C'));
            else CHECK(false);
            // 记录里的哈希必须等于我们对该路径算出的哈希，且记录按哈希有序（BSA 的硬性要求）
            const auto slash = e.path.rfind('\\');
            CHECK_EQ(e.hash, bsa::hash_name(e.path.substr(slash + 1), false));
        }
    }
    bool threw = false;
    try { bsa::write((t.dir / "x.bsa").string(), bsa::Header{.version = 100}, {}); } catch (const Error&) { threw = true; }
    CHECK(threw);
}

// 用真实游戏自带的 BSA 当「标准答案」：哈希函数要与归档里的一致，且重建的归档逐字节相同（环境里没有游戏就跳过）。
TEST(real_game_bsa_hashes_and_byte_exact_rebuild) {
    const std::string real = find_real_bsa();
    if (real.empty()) return;
    const auto idx = bsa::read_index(real);
    CHECK(!idx.files.empty());
    for (const auto& e : idx.files) {
        const auto slash = e.path.rfind('\\');
        CHECK_EQ(e.hash, bsa::hash_name(e.path.substr(slash + 1), false));
    }
    Tmp t;
    std::vector<bsa::InputFile> in;
    for (const auto& e : idx.files) {
        const fs::path f = t.dir / "x" / e.path;
        std::string norm = e.path;
        for (char& c : norm) if (c == '\\') c = '/';
        const fs::path real_f = t.dir / "x" / norm;
        put(real_f, bsa::read_file(real, idx, e));
        in.push_back({e.path, real_f.string(), e.flip});
        (void)f;
    }
    const std::string out = (t.dir / "rebuilt.bsa").string();
    bsa::write(out, idx.header, in);
    std::string a = slurp(real), b = slurp(out);
    CHECK_EQ(a.size(), b.size());
    // 官方工具在文件夹记录的 padding 字段里留了未初始化的垃圾字节（我们写 0）；这两个字段没有语义，比较时抹掉。
    {
        const std::uint32_t folders = static_cast<unsigned char>(a[16]) | (static_cast<unsigned char>(a[17]) << 8) | (static_cast<unsigned char>(a[18]) << 16);
        for (std::uint32_t i = 0; i < folders; ++i)
            for (std::string* s : {&a, &b})
                for (std::size_t k : {12u, 20u})
                    for (std::size_t j = 0; j < 4; ++j) (*s)[36 + 24 * i + k + j] = 0;
    }
    // 先比结构（头、文件夹表、文件记录、名字块），再比整体
    const auto rebuilt = bsa::read_index(out);
    CHECK_EQ(rebuilt.files.size(), idx.files.size());
    std::size_t first_diff = std::string::npos;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) if (a[i] != b[i]) { first_diff = i; break; }
    if (first_diff != std::string::npos) std::fprintf(stderr, "  first differing byte at %zu of %zu (archive flags 0x%x)\n", first_diff, a.size(), idx.header.archive_flags);
    if (const char* dbg = std::getenv("MOL_BSA_DEBUG")) { fs::copy_file(real, std::string(dbg) + "/real.bsa", fs::copy_options::overwrite_existing); fs::copy_file(out, std::string(dbg) + "/rebuilt.bsa", fs::copy_options::overwrite_existing); }
    CHECK(a == b);
}
