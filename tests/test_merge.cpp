// WP1 验收测试：casefold + scan_layer + merge_listings。
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minitest.hpp"

#include "mol/casefold.hpp"
#include "mol/merge.hpp"

namespace {

// ------------------------------------------------------------------ 测试基建

void push_entry(mol::vector<mol::ScanEntry>& entries, std::string_view rel, bool is_dir,
               std::string_view abs) {
    mol::ScanEntry e(mol::allocator_type(entries.get_allocator().resource()));
    e.rel.assign(rel);
    e.is_dir = is_dir;
    e.abs.assign(abs);
    entries.push_back(std::move(e));
}

struct Layer {
    mol::vector<mol::ScanEntry> entries;
    explicit Layer(mol::mr* mem = mol::default_mr()) : entries(mol::allocator_type(mem)) {}
    Layer& file(std::string_view rel, std::string_view abs) {
        push_entry(entries, rel, false, abs);
        return *this;
    }
    Layer& dir(std::string_view rel, std::string_view abs) {
        push_entry(entries, rel, true, abs);
        return *this;
    }
};

using Layers = mol::vector<mol::vector<mol::ScanEntry>>;

Layers pack(mol::mr* mem, std::initializer_list<Layer> items) {
    const mol::allocator_type alloc(mem);
    Layers layers(alloc);
    for (const Layer& l : items) layers.push_back(l.entries);
    return layers;
}

std::string dump(const mol::MergeResult& r) {
    std::string s;
    for (const mol::MergedEntry& e : r.entries) {
        s += "entry ";
        s += std::string(e.path);
        s += e.is_dir ? " dir " : " file ";
        s += "layer=";
        s += std::to_string(e.layer);
        s += " src=";
        s += std::string(e.source);
        s += "\n";
    }
    for (const mol::Conflict& c : r.conflicts) {
        s += "conflict ";
        s += std::string(c.path);
        s += " winner=";
        s += std::to_string(c.winner);
        s += " losers=[";
        for (std::size_t i = 0; i != c.losers.size(); ++i) {
            if (i != 0) s += ",";
            s += std::to_string(c.losers[i]);
        }
        s += "]\n";
    }
    for (const mol::Warning& w : r.warnings) {
        s += "warning ";
        s += std::string(w.path);
        s += " ";
        s += std::string(w.message);
        s += "\n";
    }
    return s;
}

const mol::MergedEntry* find_entry(const mol::MergeResult& r, std::string_view path) {
    for (const mol::MergedEntry& e : r.entries) {
        if (std::string_view(e.path) == path) return &e;
    }
    return nullptr;
}

bool is_sorted_by_path(const mol::MergeResult& r) {
    for (std::size_t i = 1; i != r.entries.size(); ++i) {
        if (std::string_view(r.entries[i - 1].path) < std::string_view(r.entries[i].path)) continue;
        return false;
    }
    return true;
}

int& dir_counter() {
    static int n = 0;
    return n;
}

// 唯一临时目录；只落在 /tmp 下，离开作用域即清理。
struct TempDir {
    std::filesystem::path p;
    TempDir() {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        CHECK(base.string().rfind("/tmp", 0) == 0);
        p = base / ("mol_wp1_merge_" + std::to_string(getpid()) + "_" +
                    std::to_string(++dir_counter()));
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        CHECK(!ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
    }
    std::filesystem::path sub(std::string_view rel) const {
        std::filesystem::path q = p;
        std::size_t i = 0;
        while (i < rel.size()) {
            const std::size_t j = rel.find('/', i);
            const std::size_t end = j == std::string_view::npos ? rel.size() : j;
            if (end > i) q /= std::filesystem::path(std::string(rel.substr(i, end - i)));
            if (j == std::string_view::npos) break;
            i = j + 1;
        }
        return q;
    }
    void touch(std::string_view rel) const {
        const std::filesystem::path q = sub(rel);
        std::error_code ec;
        std::filesystem::create_directories(q.parent_path(), ec);
        FILE* f = std::fopen(q.string().c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) std::fclose(f);
    }
    void mkdir(std::string_view rel) const {
        std::error_code ec;
        std::filesystem::create_directories(sub(rel), ec);
    }
};

// 计数型 memory_resource：记录所有经它分配的块，供「字符串确实来自该 resource」验证。
class RecordingMr final : public mol::mr {
public:
    std::size_t allocs = 0;
    std::vector<std::pair<const std::byte*, std::size_t>> blocks;

    void* do_allocate(std::size_t bytes, std::size_t align) override {
        ++allocs;
        void* p = std::pmr::new_delete_resource()->allocate(bytes, align);
        blocks.emplace_back(static_cast<const std::byte*>(p), bytes);
        return p;
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t align) noexcept override {
        std::pmr::new_delete_resource()->deallocate(p, bytes, align);
    }
    bool do_is_equal(const mol::mr& other) const noexcept override { return this == &other; }

    bool owns(const void* p) const {
        const auto* b = static_cast<const std::byte*>(p);
        for (const auto& blk : blocks) {
            if (b >= blk.first && b < blk.first + blk.second) return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------- 测试

TEST(casefold_ascii_only) {
    CHECK_EQ(std::string(mol::casefold("ABC-DEF_123.DDS")), std::string("abc-def_123.dds"));
    CHECK_EQ(std::string(mol::casefold("")), std::string(""));
    CHECK_EQ(std::string(mol::casefold("Data/Textures/Foo.DDS")),
             std::string("data/textures/foo.dds"));
    // 非 ASCII 字节原样保留
    const std::string raw = "Data/\xc3\x9cML\xc3\xa4ut.DDS";
    CHECK(mol::casefold(raw) == "data/\xc3\x9cml\xc3\xa4ut.dds");
}

TEST(html_unescape_names) {
    CHECK_EQ(std::string(mol::html_unescape("JK&#39;s Skyhaven forge V1")), std::string("JK's Skyhaven forge V1"));
    CHECK_EQ(std::string(mol::html_unescape("A &amp; B &lt;x&gt; &quot;q&quot; &apos;")), std::string("A & B <x> \"q\" '"));
    CHECK_EQ(std::string(mol::html_unescape("&#x4E2D;&#25991;")), std::string("\u4E2D\u6587"));
    // 不认识的、不完整的、越界的保持原样
    CHECK_EQ(std::string(mol::html_unescape("R&D &bogus; &#; &#xZZ; & tail &#1114112;")), std::string("R&D &bogus; &#; &#xZZ; & tail &#1114112;"));
}

TEST(casefold_uses_supplied_resource) {
    RecordingMr rec;
    mol::string s = mol::casefold("a long enough path to defeat short string optimization", &rec);
    CHECK(rec.allocs > 0);
    CHECK(rec.owns(s.data()));
    CHECK_EQ(std::string(s), std::string("a long enough path to defeat short string optimization"));
}

TEST(merge_case_insensitive_same_directory) {
    // 大小写不同的同名目录合并为一个，规范大小写取层 0。
    Layer l0;
    l0.file("Textures/foo.dds", "/m0/Textures/foo.dds");
    Layer l1;
    l1.file("textures/BAR.dds", "/m1/textures/BAR.dds");

    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {l0, l1}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{3});
    CHECK(is_sorted_by_path(r));
    CHECK_EQ(dump(r), std::string(
                          "entry Textures dir layer=0 src=\n"
                          "entry Textures/BAR.dds file layer=1 src=/m1/textures/BAR.dds\n"
                          "entry Textures/foo.dds file layer=0 src=/m0/Textures/foo.dds\n"));
    CHECK(r.conflicts.empty());
    CHECK(r.warnings.empty());
}

TEST(merge_case_insensitive_file_highest_layer_wins) {
    Layer l0;
    l0.file("Data/x.dds", "/m0/Data/x.dds");
    Layer l1;
    l1.file("data/X.DDS", "/m1/data/X.DDS");

    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {l0, l1}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{2});
    CHECK_EQ(dump(r), std::string(
                          "entry Data dir layer=0 src=\n"
                          "entry Data/x.dds file layer=1 src=/m1/data/X.DDS\n"
                          "conflict Data/x.dds winner=1 losers=[0]\n"));
    CHECK_EQ(r.conflicts.size(), std::size_t{1});
    CHECK_EQ(std::string(r.conflicts[0].path), std::string("Data/x.dds"));
    CHECK_EQ(r.conflicts[0].winner, std::size_t{1});
    CHECK_EQ(r.conflicts[0].losers.size(), std::size_t{1});
    CHECK_EQ(r.conflicts[0].losers[0], std::size_t{0});
    CHECK(r.warnings.empty());
}

TEST(merge_layer0_case_is_canonical) {
    // 层 0（最先引入）的原始大小写决定规范路径，即使它输给更高层的文件。
    Layer l0;
    l0.file("Data/Textures/A.DDS", "/m0/Data/Textures/A.DDS");
    Layer l1;
    l1.file("data/textures/a.dds", "/m1/data/textures/a.dds");

    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {l0, l1}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{3});
    CHECK_EQ(dump(r), std::string(
                          "entry Data dir layer=0 src=\n"
                          "entry Data/Textures dir layer=0 src=\n"
                          "entry Data/Textures/A.DDS file layer=1 src=/m1/data/textures/a.dds\n"
                          "conflict Data/Textures/A.DDS winner=1 losers=[0]\n"));
    CHECK_EQ(r.conflicts.size(), std::size_t{1});
    CHECK_EQ(std::string(r.conflicts[0].path), std::string("Data/Textures/A.DDS"));
    CHECK(r.warnings.empty());
}

TEST(merge_dir_loses_to_file_in_higher_layer) {
    // 更高层把目录变成文件：目录条目与其子树全部丢弃，产生 Warning。
    Layer l0;
    l0.file("Data/Textures/a.dds", "/m0/Data/Textures/a.dds");
    Layer l2;  // 注意：层 1 不存在
    l2.file("data/TEXTURES", "/m2/data/TEXTURES");

    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {l0, Layer(), l2}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{2});
    CHECK_EQ(dump(r), std::string(
                          "entry Data dir layer=0 src=\n"
                          "entry Data/Textures file layer=2 src=/m2/data/TEXTURES\n"
                          "warning Data/Textures dir/file kind mismatch between layer 0 and layer 2\n"));
    CHECK(r.conflicts.empty());
    CHECK_EQ(r.warnings.size(), std::size_t{1});
    CHECK_EQ(std::string(r.warnings[0].path), std::string("Data/Textures"));
    CHECK_EQ(std::string(r.warnings[0].message),
             std::string("dir/file kind mismatch between layer 0 and layer 2"));
}

TEST(merge_file_becomes_dir_in_higher_layer) {
    // 更高层在同一个 key 上放目录：文件条目被否决，产生 Warning。
    Layer l0;
    l0.file("Data/Textures", "/m0/Data/Textures");
    Layer l1;
    l1.file("data/textures/b.dds", "/m1/data/textures/b.dds");

    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {l0, l1}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{3});
    CHECK_EQ(dump(r), std::string(
                          "entry Data dir layer=0 src=\n"
                          "entry Data/Textures dir layer=1 src=\n"
                          "entry Data/Textures/b.dds file layer=1 src=/m1/data/textures/b.dds\n"
                          "warning Data/Textures dir/file kind mismatch between layer 0 and layer 1\n"));
    CHECK(r.conflicts.empty());
    CHECK_EQ(r.warnings.size(), std::size_t{1});
    CHECK_EQ(std::string(r.warnings[0].message),
             std::string("dir/file kind mismatch between layer 0 and layer 1"));
}

TEST(merge_implicit_parent_directories) {
    Layer l0;
    l0.file("Data/Textures/a.dds", "/m0/Data/Textures/a.dds");

    const mol::MergeResult r = mol::merge_listings(pack(mol::default_mr(), {l0}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{3});
    CHECK_EQ(dump(r), std::string(
                          "entry Data dir layer=0 src=\n"
                          "entry Data/Textures dir layer=0 src=\n"
                          "entry Data/Textures/a.dds file layer=0 src=/m0/Data/Textures/a.dds\n"));
    CHECK(r.conflicts.empty());
    CHECK(r.warnings.empty());
}

TEST(merge_explicit_dir_entry_supplies_source) {
    Layer l0;
    l0.file("Data/Textures/a.dds", "/m0/Data/Textures/a.dds");
    Layer l1;
    l1.dir("Data/Textures", "/m1/Data/Textures");

    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {l0, l1}), mol::default_mr());
    CHECK_EQ(dump(r), std::string(
                          "entry Data dir layer=0 src=\n"
                          "entry Data/Textures dir layer=0 src=/m1/Data/Textures\n"
                          "entry Data/Textures/a.dds file layer=0 src=/m0/Data/Textures/a.dds\n"));
    CHECK(r.conflicts.empty());
    CHECK(r.warnings.empty());
}

TEST(merge_standalone_explicit_dir_keeps_source) {
    // 只有显式目录条目（无子条目）时也要输出，source 为该层真实路径。
    Layer l0;
    l0.dir("Data/Empty", "/m0/Data/Empty");

    const mol::MergeResult r = mol::merge_listings(pack(mol::default_mr(), {l0}), mol::default_mr());
    CHECK_EQ(dump(r), std::string(
                          "entry Data dir layer=0 src=\n"
                          "entry Data/Empty dir layer=0 src=/m0/Data/Empty\n"));
}

TEST(merge_intra_layer_casefold_conflict) {
    Layer l0;
    l0.file("Data/a.dds", "/m0/Data/a.dds");
    l0.file("data/A.dds", "/m0/data/A.dds");

    const mol::MergeResult r = mol::merge_listings(pack(mol::default_mr(), {l0}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{2});
    CHECK_EQ(dump(r), std::string(
                          "entry Data dir layer=0 src=\n"
                          "entry Data/a.dds file layer=0 src=/m0/Data/a.dds\n"
                          "warning Data/a.dds intra-layer casefold conflict in layer 0: winner "
                          "'Data/a.dds' shadows loser 'data/A.dds'\n"));
    CHECK(r.conflicts.empty());
    CHECK_EQ(r.warnings.size(), std::size_t{1});
    CHECK_EQ(std::string(r.warnings[0].path), std::string("Data/a.dds"));
    CHECK_EQ(std::string(r.warnings[0].message),
             std::string("intra-layer casefold conflict in layer 0: winner 'Data/a.dds' "
                         "shadows loser 'data/A.dds'"));
}

TEST(merge_input_order_does_not_matter) {
    // 同一层内顺序打乱（含 casefold 冲突与共享祖先的不同大小写）→ 结果不变。
    Layer a;
    a.file("Textures/foo.dds", "/m/Textures/foo.dds");
    a.file("textures/BAR.dds", "/m/textures/BAR.dds");
    a.file("Data/x.dds", "/m/Data/x.dds");
    a.file("data/X.DDS", "/m/data/X.DDS");
    a.file("Meshes/b.nif", "/m/Meshes/b.nif");

    Layer b;  // 反向
    b.file("Meshes/b.nif", "/m/Meshes/b.nif");
    b.file("data/X.DDS", "/m/data/X.DDS");
    b.file("Data/x.dds", "/m/Data/x.dds");
    b.file("textures/BAR.dds", "/m/textures/BAR.dds");
    b.file("Textures/foo.dds", "/m/Textures/foo.dds");

    Layer upper;
    upper.file("meshes/B.nif", "/u/meshes/B.nif");

    const std::string one = dump(mol::merge_listings(pack(mol::default_mr(), {a, upper}),
                                                     mol::default_mr()));
    const std::string two = dump(mol::merge_listings(pack(mol::default_mr(), {b, upper}),
                                                    mol::default_mr()));
    CHECK_EQ(one, two);
    const std::string again = dump(mol::merge_listings(pack(mol::default_mr(), {a, upper}),
                                                      mol::default_mr()));
    CHECK_EQ(one, again);
    // 冲突 / 告警也确实产生
    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {a, upper}), mol::default_mr());
    CHECK_EQ(r.conflicts.size(), std::size_t{1});  // meshes/b.nif 被 upper 层覆盖
    CHECK_EQ(r.warnings.size(), std::size_t{1});   // Data/x.dds 的层内 casefold 冲突
    CHECK(is_sorted_by_path(r));
    // 规范大小写：Data/x.dds 的 "Data" 来自 min("Data/x.dds","data/X.DDS")
    CHECK(find_entry(r, "Data/x.dds") != nullptr);
    CHECK(find_entry(r, "Textures/BAR.dds") != nullptr);
}

TEST(merge_conflict_losers_are_ascending) {
    Layer l0;
    l0.file("x.dds", "/0/x.dds");
    Layer l2;
    l2.file("X.DDS", "/2/X.DDS");
    Layer l4;
    l4.file("x.dds", "/4/x.dds");

    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {l0, Layer(), l2, Layer(), l4}),
                            mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{1});
    CHECK_EQ(std::string(r.entries[0].path), std::string("x.dds"));
    CHECK_EQ(r.entries[0].layer, std::size_t{4});
    CHECK_EQ(std::string(r.entries[0].source), std::string("/4/x.dds"));
    CHECK_EQ(r.conflicts.size(), std::size_t{1});
    CHECK_EQ(r.conflicts[0].winner, std::size_t{4});
    CHECK_EQ(r.conflicts[0].losers.size(), std::size_t{2});
    CHECK_EQ(r.conflicts[0].losers[0], std::size_t{0});
    CHECK_EQ(r.conflicts[0].losers[1], std::size_t{2});
    CHECK(r.warnings.empty());
}

TEST(merge_empty_inputs) {
    const mol::MergeResult none = mol::merge_listings({}, mol::default_mr());
    CHECK(none.entries.empty());
    CHECK(none.conflicts.empty());
    CHECK(none.warnings.empty());

    Layer zero;
    zero.file("a/b/c.dds", "/0/a/b/c.dds");
    const mol::MergeResult r =
        mol::merge_listings(pack(mol::default_mr(), {Layer(), zero, Layer()}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{3});  // a, a/b, a/b/c.dds
    CHECK_EQ(std::string(r.entries[0].path), std::string("a"));
    CHECK_EQ(std::string(r.entries[2].path), std::string("a/b/c.dds"));
}

TEST(merge_skips_empty_rel) {
    Layer l0;
    l0.file("", "/0/ignored");
    l0.file("Data/ok.dds", "/0/Data/ok.dds");
    const mol::MergeResult r = mol::merge_listings(pack(mol::default_mr(), {l0}), mol::default_mr());
    CHECK_EQ(r.entries.size(), std::size_t{2});
    CHECK(find_entry(r, "ignored") == nullptr);
}

TEST(merge_results_are_deterministic) {
    Layer l0;
    l0.file("Data/Textures/a.dds", "/m0/Data/Textures/a.dds");
    Layer l1;
    l1.file("data/TEXTURES/b.dds", "/m1/data/TEXTURES/b.dds");
    l1.file("Data/Textures/c.dds", "/m1/Data/Textures/c.dds");
    const Layers layers = pack(mol::default_mr(), {l0, l1});
    const std::string one = dump(mol::merge_listings(layers, mol::default_mr()));
    const std::string two = dump(mol::merge_listings(layers, mol::default_mr()));
    CHECK_EQ(one, two);
}

TEST(merge_allocates_from_supplied_resource) {
    // 长路径（超出 SSO）→ entries/warnings 的字符串必须来自调用方传入的 resource。
    const std::string deep_a = "Data/VeryLongDirectoryName/That/Keeps/Going/textures/a_textures.dds";
    const std::string deep_b = "Data/VeryLongDirectoryName/That/Keeps/Going/textures/b_textures.dds";

    Layer l0;
    l0.file(deep_a, "/m0/Data/VeryLongDirectoryName/That/Keeps/Going/textures/a_textures.dds");
    Layer l1;
    l1.file(deep_b, "/m1/Data/VeryLongDirectoryName/That/Keeps/Going/textures/b_textures.dds");
    l1.file("Data/VeryLongDirectoryName/That/Keeps/Going/textures/a_textures.dds",
            "/m1/Data/VeryLongDirectoryName/That/Keeps/Going/textures/a_textures.dds");

    RecordingMr rec;
    const Layers layers = pack(mol::default_mr(), {l0, l1});
    const mol::MergeResult r = mol::merge_listings(layers, &rec);
    CHECK(rec.allocs > 0);
    CHECK_EQ(r.entries.size(), std::size_t{8});  // 6 个目录 + 2 个文件
    for (const mol::MergedEntry& e : r.entries) {
        CHECK(rec.owns(e.path.data()));
        if (!e.source.empty()) CHECK(rec.owns(e.source.data()));
    }
    CHECK_EQ(r.conflicts.size(), std::size_t{1});
    CHECK(rec.owns(r.conflicts[0].path.data()));
    CHECK(find_entry(r, deep_b) != nullptr);
}

TEST(scan_layer_basic) {
    TempDir tmp;
    tmp.touch("Textures/foo.dds");
    tmp.touch("Data.txt");
    tmp.touch(".hidden");
    tmp.touch("skse/plugins/plugin.dll");
    std::error_code ec;
    std::filesystem::create_directory_symlink(tmp.sub("skse"), tmp.sub("linkdir"), ec);
    CHECK(!ec);
    std::filesystem::create_directory_symlink(tmp.sub("nope"), tmp.sub("dangle"), ec);
    CHECK(!ec);
    std::filesystem::create_symlink(tmp.sub("Data.txt"), tmp.sub("linkfile"), ec);
    CHECK(!ec);

    const mol::vector<mol::ScanEntry> got = mol::scan_layer(tmp.p.string(), "Data", mol::default_mr());
    CHECK_EQ(got.size(), std::size_t{10});
    const char* names[10] = {".hidden", "Data.txt", "Textures", "Textures/foo.dds", "dangle",
                             "linkdir", "linkfile", "skse", "skse/plugins",
                             "skse/plugins/plugin.dll"};
    for (int i = 0; i < 10; ++i) {
        const std::string want_rel = std::string("Data/") + names[i];
        CHECK_EQ(std::string(got[static_cast<std::size_t>(i)].rel), want_rel);
        CHECK_EQ(std::string(got[static_cast<std::size_t>(i)].abs),
                 (tmp.sub(names[i])).string());
    }
    // 目录符号链接不递归
    for (const mol::ScanEntry& e : got) {
        CHECK(e.rel.find("linkdir/") == std::string_view::npos);
    }
}

TEST(scan_layer_types_and_top_only) {
    TempDir tmp;
    tmp.touch("Textures/foo.dds");
    tmp.touch("a.esp");
    std::error_code ec;
    std::filesystem::create_directory_symlink(tmp.sub("Textures"), tmp.sub("linkdir"), ec);
    std::filesystem::create_directory_symlink(tmp.sub("nope"), tmp.sub("dangle"), ec);
    std::filesystem::create_symlink(tmp.sub("a.esp"), tmp.sub("linkfile"), ec);
    const auto all = mol::scan_layer(tmp.p.string(), {}, mol::default_mr());
    auto kind = [&](const mol::vector<mol::ScanEntry>& v, std::string_view rel) -> int {
        for (const auto& e : v) if (e.rel == rel) return e.is_dir ? 1 : 0;
        return -1;
    };
    CHECK_EQ(kind(all, "linkdir"), 1);    // 指向目录的链接 → 目录（但不递归）
    CHECK_EQ(kind(all, "dangle"), 0);     // 断链 → 文件
    CHECK_EQ(kind(all, "linkfile"), 0);
    CHECK_EQ(kind(all, "Textures"), 1);
    CHECK_EQ(kind(all, "Textures/foo.dds"), 0);
    CHECK_EQ(kind(all, "linkdir/foo.dds"), -1);

    const auto top = mol::scan_layer_top(tmp.p.string(), "Data", mol::default_mr());
    CHECK_EQ(top.size(), std::size_t{5});  // Textures、a.esp、三个链接；不含 Textures/foo.dds
    CHECK_EQ(kind(top, "Data/Textures"), 1);
    CHECK_EQ(kind(top, "Data/Textures/foo.dds"), -1);
    CHECK_EQ(std::string(top[1].abs), tmp.sub("a.esp").string());
    // 根以 '/' 结尾也不产生双斜杠
    const auto slash = mol::scan_layer_top(tmp.p.string() + "/", {}, mol::default_mr());
    CHECK_EQ(std::string(slash[1].abs), tmp.sub("a.esp").string());
}

TEST(scan_layer_no_prefix_and_missing_root) {
    TempDir tmp;
    tmp.touch("a/b.dds");
    {
        const mol::vector<mol::ScanEntry> got =
            mol::scan_layer(tmp.p.string(), {}, mol::default_mr());
        CHECK_EQ(got.size(), std::size_t{2});
        CHECK_EQ(std::string(got[0].rel), std::string("a"));
        CHECK(got[0].is_dir);
        CHECK_EQ(std::string(got[1].rel), std::string("a/b.dds"));
        CHECK(!got[1].is_dir);
        CHECK(got[0].rel.front() != '/');
    }
    // prefix 带斜杠 → 归一化，且不为 prefix 本身生成条目
    {
        const mol::vector<mol::ScanEntry> got =
            mol::scan_layer(tmp.p.string(), "/Data/", mol::default_mr());
        CHECK_EQ(got.size(), std::size_t{2});
        CHECK_EQ(std::string(got[0].rel), std::string("Data/a"));
        CHECK_EQ(std::string(got[1].rel), std::string("Data/a/b.dds"));
        for (const mol::ScanEntry& e : got) CHECK(e.rel != "Data");
    }
    // 不存在的 root
    const mol::vector<mol::ScanEntry> missing =
        mol::scan_layer((tmp.p / "does_not_exist").string(), "Data", mol::default_mr());
    CHECK(missing.empty());
    // root 是普通文件
    const mol::vector<mol::ScanEntry> as_file = mol::scan_layer(tmp.p.string() + "/a/b.dds", {},
                                                                mol::default_mr());
    CHECK(as_file.empty());
}

TEST(scan_then_merge_end_to_end) {
    TempDir game;
    game.touch("Data/Textures/a.dds");
    game.touch("Data/Textures/lower.dds");
    game.mkdir("Data/Interface");

    TempDir mod;
    mod.touch("Textures/A.DDS");      // 与层 0 的大小写不同但折叠同名 → 冲突
    mod.touch("Textures/extra.dds");  // 新文件
    mod.touch("Meshes/m.nif");        // 全新目录

    const mol::vector<mol::ScanEntry> l0 =
        mol::scan_layer(game.sub("Data").string(), "Data", mol::default_mr());
    const mol::vector<mol::ScanEntry> l1 =
        mol::scan_layer(mod.p.string(), "Data", mol::default_mr());

    const mol::allocator_type alloc = mol::allocator_type(mol::default_mr());
    Layers layers(alloc);
    layers.push_back(l0);
    layers.push_back(l1);
    const mol::MergeResult r = mol::merge_listings(layers, mol::default_mr());

    CHECK_EQ(r.entries.size(), std::size_t{8});
    CHECK(is_sorted_by_path(r));
    CHECK_EQ(std::string(r.entries[0].path), std::string("Data"));
    CHECK_EQ(std::string(r.entries[1].path), std::string("Data/Interface"));
    CHECK_EQ(std::string(r.entries[2].path), std::string("Data/Meshes"));
    CHECK_EQ(std::string(r.entries[3].path), std::string("Data/Meshes/m.nif"));
    CHECK_EQ(std::string(r.entries[4].path), std::string("Data/Textures"));
    CHECK_EQ(std::string(r.entries[5].path), std::string("Data/Textures/a.dds"));
    CHECK_EQ(r.entries[5].layer, std::size_t{1});
    CHECK_EQ(std::string(r.entries[5].source), (mod.sub("Textures/A.DDS")).string());
    CHECK_EQ(std::string(r.entries[6].path), std::string("Data/Textures/extra.dds"));
    CHECK_EQ(std::string(r.entries[6].source), (mod.sub("Textures/extra.dds")).string());
    CHECK_EQ(std::string(r.entries[7].path), std::string("Data/Textures/lower.dds"));
    CHECK_EQ(r.entries[7].layer, std::size_t{0});
    CHECK_EQ(std::string(r.entries[7].source), (game.sub("Data/Textures/lower.dds")).string());
    CHECK_EQ(r.conflicts.size(), std::size_t{1});
    CHECK_EQ(std::string(r.conflicts[0].path), std::string("Data/Textures/a.dds"));
    CHECK_EQ(r.conflicts[0].winner, std::size_t{1});
    CHECK_EQ(r.conflicts[0].losers.size(), std::size_t{1});
    CHECK(r.warnings.empty());
}

}  // namespace
