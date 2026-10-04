// WP3 测试：链接农场。全部在 temp_directory_path() 下的唯一子目录里工作，结尾清理。
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "minitest.hpp"

#include "mol/linkfarm.hpp"
#include "mol/merge.hpp"

import alib6;


namespace fs = std::filesystem;

using mol::MergedEntry;
using mol::MergeResult;
using mol::OpKind;

namespace {

int g_seq = 0;

// 唯一临时目录，析构时 remove_all。
struct Scratch {
    fs::path dir;
    Scratch() {
        const fs::path base = fs::temp_directory_path();
        const std::string b = base.generic_string();
        CHECK(b.rfind("/tmp", 0) == 0 || b.rfind("/var/folders", 0) == 0);
        for (int i = 0; i < 100000; ++i) {
            fs::path p = base / ("mol_linkfarm-" + std::to_string(++g_seq));
            std::error_code ec;
            if (fs::create_directory(p, ec)) {
                dir = p;
                return;
            }
        }
        CHECK(false);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void write_file(const fs::path& p, std::string_view content = "x") {
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
    std::ofstream os(p, std::ios::binary);
    os << content;
    CHECK(static_cast<bool>(os));
}

std::string sp(const fs::path& p) { return p.generic_string(); }

// 手工构造 MergeResult（不依赖 WP1 的 scan/merge）。
struct Ent {
    std::string path;
    bool dir;
    std::string src;
};

MergeResult make_result(const std::vector<Ent>& l) {
    MergeResult r;
    for (const auto& e : l) {
        MergedEntry m;
        m.path = e.path;
        m.is_dir = e.dir;
        m.source = e.src;
        r.entries.push_back(std::move(m));
    }
    return r;
}

// manifest 的解析结果（用 alib6 AData 读，和被测代码同一套 JSON 实现）。
struct Manifest {
    int version = 0;
    std::vector<std::string> created;
};

Manifest read_manifest(const fs::path& farm) {
    std::ifstream in(farm / mol::kFarmMarker, std::ios::binary);
    CHECK(static_cast<bool>(in));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    alib6::AData doc;
    CHECK(doc.load_from_memory(text));
    Manifest m;
    CHECK(doc.is_object());
    const auto& obj = doc.object();
    auto v = obj.find("version");
    auto c = obj.find("created");
    CHECK(v != obj.end() && c != obj.end());
    if (v != obj.end()) m.version = v.second().to<int>();
    if (c != obj.end() && c.second().is_array())
        for (const auto& e : c.second().array()) m.created.emplace_back(e.to<std::string_view>());
    return m;
}

std::string op_str(const mol::Op& op) {
    const char* k = "?";
    switch (op.kind) {
        case OpKind::Mkdir: k = "Mkdir"; break;
        case OpKind::Link: k = "Link"; break;
        case OpKind::Relink: k = "Relink"; break;
        case OpKind::Remove: k = "Remove"; break;
        case OpKind::Rmdir: k = "Rmdir"; break;
    }
    std::string s = std::string(k) + " " + std::string(op.path);
    if (!op.target.empty()) s += " -> " + std::string(op.target);
    return s;
}

std::string ops_str(const mol::Plan& p) {
    std::string s;
    for (const auto& op : p.ops) s += op_str(op) + "\n";
    return s;
}

// ---------------------------------------------------------------- 1

TEST(apply_farm_creates_tree_and_is_idempotent) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    write_file(src / "Textures" / "t.dds", "t");
    const fs::path farm = s.dir / "farm";
    const std::string a_src = sp(src / "a.dds");
    const std::string t_src = sp(src / "Textures" / "t.dds");

    const MergeResult r = make_result({
        {"Data", true, ""},
        {"Data/Textures", true, ""},
        {"Data/a.dds", false, a_src},
        {"Data/Textures/t.dds", false, t_src},
    });

    mol::Plan plan = mol::plan_farm(r, sp(farm));
    CHECK(!plan.empty());
    mol::apply_farm(plan, sp(farm));

    // 结构：真实目录 + 指向 source 原样绝对路径的符号链接
    CHECK(fs::is_directory(farm / "Data"));
    CHECK(fs::is_directory(farm / "Data" / "Textures"));
    CHECK(fs::is_symlink(farm / "Data" / "a.dds"));
    CHECK(fs::is_symlink(farm / "Data" / "Textures" / "t.dds"));
    CHECK_EQ(sp(fs::read_symlink(farm / "Data" / "a.dds")), a_src);
    CHECK_EQ(sp(fs::read_symlink(farm / "Data" / "Textures" / "t.dds")), t_src);
    // 游戏本体目录没被动过
    CHECK(fs::is_regular_file(src / "a.dds"));

    // manifest：version 1 + created 按字节序排序、且不含 marker 自身
    const Manifest j = read_manifest(farm);
    CHECK_EQ(j.version, 1);
    CHECK_EQ(j.created.size(), static_cast<std::size_t>(4));
    const std::vector<std::string> expect_created = {"Data", "Data/Textures", "Data/Textures/t.dds",
                                                     "Data/a.dds"};
    for (std::size_t i = 0; i < expect_created.size(); ++i)
        CHECK_EQ(j.created[i], expect_created[i]);
    // 没有留下临时文件
    CHECK(!fs::exists(farm / (std::string(mol::kFarmMarker) + ".tmp")));

    // 幂等：同一 expected 再 plan 必为空，再 apply 也不改变任何东西
    const mol::Plan plan2 = mol::plan_farm(r, sp(farm));
    CHECK(plan2.empty());
    mol::apply_farm(plan2, sp(farm));
    const mol::Plan plan3 = mol::plan_farm(r, sp(farm));
    CHECK(plan3.empty());
    CHECK(fs::is_symlink(farm / "Data" / "a.dds"));
}

// ---------------------------------------------------------------- 2

TEST(source_change_triggers_relink) {
    Scratch s;
    const fs::path src1 = s.dir / "src1";
    const fs::path src2 = s.dir / "src2";
    write_file(src1 / "a.dds", "one");
    write_file(src2 / "a.dds", "two");
    const fs::path farm = s.dir / "farm";

    const MergeResult r1 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src1 / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r1, sp(farm)), sp(farm));
    CHECK_EQ(sp(fs::read_symlink(farm / "Data" / "a.dds")), sp(src1 / "a.dds"));

    const MergeResult r2 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src2 / "a.dds")},
    });
    mol::Plan plan = mol::plan_farm(r2, sp(farm));
    CHECK_EQ(plan.ops.size(), static_cast<std::size_t>(1));
    CHECK_EQ(op_str(plan.ops[0]),
             "Relink Data/a.dds -> " + sp(src2 / "a.dds"));
    mol::apply_farm(plan, sp(farm));
    CHECK_EQ(sp(fs::read_symlink(farm / "Data" / "a.dds")), sp(src2 / "a.dds"));
    // 收敛
    CHECK(mol::plan_farm(r2, sp(farm)).empty());
}

// ---------------------------------------------------------------- 3

TEST(removed_entries_trigger_remove_and_rmdir) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    write_file(src / "x.dds", "x");
    const fs::path farm = s.dir / "farm";

    const MergeResult r1 = make_result({
        {"Data", true, ""},
        {"Data/Sub", true, ""},
        {"Data/Sub/x.dds", false, sp(src / "x.dds")},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r1, sp(farm)), sp(farm));
    CHECK(fs::is_directory(farm / "Data" / "Sub"));

    const MergeResult r2 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::Plan plan = mol::plan_farm(r2, sp(farm));
    CHECK_EQ(ops_str(plan), "Remove Data/Sub/x.dds\nRmdir Data/Sub\n");
    mol::apply_farm(plan, sp(farm));
    CHECK(!fs::exists(farm / "Data" / "Sub"));
    CHECK(fs::is_symlink(farm / "Data" / "a.dds"));
    CHECK(fs::is_directory(farm / "Data"));
    // manifest 同步收敛，再 plan 为空
    const Manifest j = read_manifest(farm);
    CHECK_EQ(j.created.size(), static_cast<std::size_t>(2));
    CHECK_EQ(j.created[0], "Data");
    CHECK_EQ(j.created[1], "Data/a.dds");
    CHECK(mol::plan_farm(r2, sp(farm)).empty());
}

// ---------------------------------------------------------------- 4

TEST(user_file_in_root_rejected) {
    Scratch s;
    const fs::path farm = s.dir / "farm";
    fs::create_directories(farm);
    write_file(farm / "user.txt", "mine");

    const MergeResult r = make_result({{"Data", true, ""}});
    bool threw = false;
    try {
        (void)mol::plan_farm(r, sp(farm));
    } catch (const std::runtime_error& e) {
        threw = true;
        const std::string msg = e.what();
        CHECK(msg.find(sp(farm)) != std::string::npos);
        CHECK(msg.find(mol::kFarmMarker) != std::string::npos);
    }
    CHECK(threw);
    // 用户目录没被碰
    CHECK(fs::is_regular_file(farm / "user.txt"));
    CHECK(!fs::exists(farm / mol::kFarmMarker));
}

// ---------------------------------------------------------------- 5

TEST(empty_root_and_missing_root_are_farms) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    const fs::path farm = s.dir / "farm";

    // 5a: root 不存在 → 视为空农场
    {
        const MergeResult r = make_result({
            {"Data", true, ""},
            {"Data/a.dds", false, sp(src / "a.dds")},
        });
        mol::Plan plan = mol::plan_farm(r, sp(farm));
        CHECK_EQ(ops_str(plan), "Mkdir Data\nLink Data/a.dds -> " + sp(src / "a.dds") + "\n");
        mol::apply_farm(plan, sp(farm));
        CHECK(fs::is_directory(farm / "Data"));
        CHECK(fs::is_symlink(farm / "Data" / "a.dds"));
        CHECK(mol::plan_farm(r, sp(farm)).empty());
    }

    // 5b: root 存在但为空、无 marker → 空农场
    {
        const fs::path farm2 = s.dir / "farm2";
        fs::create_directories(farm2);
        const MergeResult r = make_result({{"Data", true, ""}});
        mol::Plan plan = mol::plan_farm(r, sp(farm2));
        CHECK_EQ(ops_str(plan), "Mkdir Data\n");
        mol::apply_farm(plan, sp(farm2));
        CHECK(fs::is_directory(farm2 / "Data"));
        CHECK(fs::exists(farm2 / mol::kFarmMarker));
    }
}

// ---------------------------------------------------------------- 6

TEST(rmdir_skips_user_content) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "x.dds", "x");
    write_file(src / "a.dds", "a");
    const fs::path farm = s.dir / "farm";

    const MergeResult r1 = make_result({
        {"Data", true, ""},
        {"Data/Sub", true, ""},
        {"Data/Sub/x.dds", false, sp(src / "x.dds")},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r1, sp(farm)), sp(farm));

    // 用户往农场里放了内容
    write_file(farm / "Data" / "Sub" / "user.txt", "mine");
    fs::create_directories(farm / "Data" / "userdir" / "inner");
    // 还放了一个指向源码目录的符号链接：不得递归进去，也不得被动到
    fs::create_directory_symlink(src, farm / "Data" / "link_to_src");

    const MergeResult r2 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::Plan plan = mol::plan_farm(r2, sp(farm));
    CHECK_EQ(ops_str(plan), "Remove Data/Sub/x.dds\n");  // Sub 不 Rmdir、Data 绝不碰
    mol::apply_farm(plan, sp(farm));

    CHECK(fs::is_regular_file(farm / "Data" / "Sub" / "user.txt"));
    CHECK(fs::is_directory(farm / "Data" / "userdir" / "inner"));
    CHECK(!fs::exists(farm / "Data" / "Sub" / "x.dds"));
    CHECK(fs::is_symlink(farm / "Data" / "a.dds"));
    // 符号链接未被递归/未被动到
    CHECK(fs::is_symlink(farm / "Data" / "link_to_src"));
    CHECK(fs::is_regular_file(src / "x.dds"));
    CHECK(fs::is_regular_file(src / "a.dds"));
    // Sub 仍留在 manifest 里，再 plan 依然为空（不会反复尝试删）
    const Manifest j = read_manifest(farm);
    bool saw_sub = false;
    for (const auto& v : j.created)
        if (v == "Data/Sub") saw_sub = true;
    CHECK(saw_sub);
    CHECK(mol::plan_farm(r2, sp(farm)).empty());
}

// ---------------------------------------------------------------- 7

TEST(manifest_conflict_is_removed_then_recreated) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    const fs::path farm = s.dir / "farm";

    const MergeResult r = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r, sp(farm)), sp(farm));

    // 我们创建过的链接被换成真实文件：路径在 manifest 里 → 先删后建
    std::error_code ec;
    fs::remove(farm / "Data" / "a.dds", ec);
    CHECK(!ec);
    write_file(farm / "Data" / "a.dds", "user file");

    mol::Plan plan = mol::plan_farm(r, sp(farm));
    CHECK_EQ(ops_str(plan), "Remove Data/a.dds\nLink Data/a.dds -> " + sp(src / "a.dds") + "\n");
    mol::apply_farm(plan, sp(farm));
    CHECK(fs::is_symlink(farm / "Data" / "a.dds"));
    CHECK_EQ(sp(fs::read_symlink(farm / "Data" / "a.dds")), sp(src / "a.dds"));
    CHECK(mol::plan_farm(r, sp(farm)).empty());
}

// ---------------------------------------------------------------- 8

TEST(user_content_where_link_expected_is_rejected) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    write_file(src / "b.dds", "b");
    const fs::path farm = s.dir / "farm";

    const MergeResult r1 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r1, sp(farm)), sp(farm));

    // 用户自己放的 b.dds（不在 manifest、不在期望里）→ 期望树新增同名链接时拒绝
    write_file(farm / "Data" / "b.dds", "user");
    const MergeResult r2 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
        {"Data/b.dds", false, sp(src / "b.dds")},
    });
    bool threw = false;
    try {
        (void)mol::plan_farm(r2, sp(farm));
    } catch (const std::runtime_error& e) {
        threw = true;
        const std::string msg = e.what();
        CHECK(msg.find("Data/b.dds") != std::string::npos);
    }
    CHECK(threw);
    CHECK(fs::is_regular_file(farm / "Data" / "b.dds"));
    CHECK(fs::is_symlink(farm / "Data" / "a.dds"));

    // 用户内容不在期望树里时被无视，plan 仍为空
    CHECK(mol::plan_farm(r1, sp(farm)).empty());
}

// ---------------------------------------------------------------- 9

TEST(apply_failure_keeps_completed_manifest) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    write_file(src / "keep.dds", "keep");
    const fs::path farm = s.dir / "farm";
    const std::string too_long = "Data/" + std::string(300, 'x');

    const MergeResult r1 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r1, sp(farm)), sp(farm));

    // 第二步：删掉 a.dds、新增 keep.dds + 一个必然创建失败的超长路径
    const MergeResult r2 = make_result({
        {"Data", true, ""},
        {"Data/keep.dds", false, sp(src / "keep.dds")},
        {too_long, false, sp(src / "keep.dds")},
    });
    mol::Plan plan = mol::plan_farm(r2, sp(farm));
    CHECK(!plan.empty());
    bool threw = false;
    try {
        mol::apply_farm(plan, sp(farm));
    } catch (const std::runtime_error& e) {
        threw = true;
        const std::string msg = e.what();
        CHECK(msg.find(too_long) != std::string::npos);
        CHECK(msg.find("errno") != std::string::npos);
    }
    CHECK(threw);

    // 已完成的部分必须留在 manifest：Data 与 Data/keep.dds 在，Data/a.dds 已删
    const Manifest j = read_manifest(farm);
    std::vector<std::string> created;
    for (const auto& v : j.created) created.push_back(v);
    const std::vector<std::string> expect = {"Data", "Data/keep.dds"};
    CHECK_EQ(created.size(), expect.size());
    for (std::size_t i = 0; i < expect.size() && i < created.size(); ++i)
        CHECK_EQ(created[i], expect[i]);
    CHECK(fs::is_symlink(farm / "Data" / "keep.dds"));
    CHECK(!fs::exists(farm / "Data" / "a.dds"));

    // 修正期望（去掉失败条目）后 plan 为空：记录没丢、也没多
    const MergeResult r3 = make_result({
        {"Data", true, ""},
        {"Data/keep.dds", false, sp(src / "keep.dds")},
    });
    CHECK(mol::plan_farm(r3, sp(farm)).empty());

    // 重新 plan 原期望：先清掉不在期望里的 keep.dds，再补齐 a.dds
    const mol::Plan again = mol::plan_farm(r1, sp(farm));
    CHECK_EQ(ops_str(again), "Remove Data/keep.dds\nLink Data/a.dds -> " + sp(src / "a.dds") + "\n");
    mol::apply_farm(again, sp(farm));
    CHECK(mol::plan_farm(r1, sp(farm)).empty());
}

// ---------------------------------------------------------------- 10

TEST(ops_are_ordered) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "c.dds", "c");
    write_file(src / "w.dds", "w");
    const fs::path farm = s.dir / "farm";

    const MergeResult r1 = make_result({
        {"A", true, ""},
        {"A/B", true, ""},
        {"A/B/c.dds", false, sp(src / "c.dds")},
    });
    mol::apply_farm(mol::plan_farm(r1, sp(farm)), sp(farm));

    const MergeResult r2 = make_result({
        {"Z", true, ""},
        {"Z/Y", true, ""},
        {"Z/Y/w.dds", false, sp(src / "w.dds")},
    });
    mol::Plan plan = mol::plan_farm(r2, sp(farm));
    CHECK_EQ(ops_str(plan),
             "Remove A/B/c.dds\n"
             "Rmdir A/B\n"
             "Rmdir A\n"
             "Mkdir Z\n"
             "Mkdir Z/Y\n"
             "Link Z/Y/w.dds -> " + sp(src / "w.dds") + "\n");
    mol::apply_farm(plan, sp(farm));
    CHECK(!fs::exists(farm / "A"));
    CHECK(fs::is_symlink(farm / "Z" / "Y" / "w.dds"));
    CHECK(mol::plan_farm(r2, sp(farm)).empty());
}

// ---------------------------------------------------------------- 11

TEST(remove_farm_cleans_everything) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    write_file(src / "Textures" / "t.dds", "t");
    const fs::path farm = s.dir / "farm";

    const MergeResult r = make_result({
        {"Data", true, ""},
        {"Data/Textures", true, ""},
        {"Data/Textures/t.dds", false, sp(src / "Textures" / "t.dds")},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r, sp(farm)), sp(farm));

    // 11a: 空 root → 连 root 一起删掉
    mol::remove_farm(sp(farm));
    CHECK(!fs::exists(farm / "Data"));
    CHECK(!fs::exists(farm / mol::kFarmMarker));
    CHECK(!fs::exists(farm));
    // 源数据完好
    CHECK(fs::is_regular_file(src / "a.dds"));
    CHECK(fs::is_regular_file(src / "Textures" / "t.dds"));

    // 11b: root 里残留用户内容 → 只删我们创建的，root 保留
    const fs::path farm2 = s.dir / "farm2";
    mol::apply_farm(mol::plan_farm(r, sp(farm2)), sp(farm2));
    write_file(farm2 / "keepme.txt", "mine");
    mol::remove_farm(sp(farm2));
    CHECK(fs::exists(farm2));
    CHECK(fs::is_regular_file(farm2 / "keepme.txt"));
    CHECK(!fs::exists(farm2 / "Data"));
    CHECK(!fs::exists(farm2 / mol::kFarmMarker));
}

// ---------------------------------------------------------------- 12

TEST(remove_farm_on_non_farm_throws) {
    Scratch s;
    const fs::path notfarm = s.dir / "notfarm";
    fs::create_directories(notfarm);
    write_file(notfarm / "x.txt", "x");
    bool threw = false;
    try {
        mol::remove_farm(sp(notfarm));
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string(e.what()).find(mol::kFarmMarker) != std::string::npos);
    }
    CHECK(threw);
    CHECK(fs::is_regular_file(notfarm / "x.txt"));
    // 不存在的 root 同样拒绝
    threw = false;
    try {
        mol::remove_farm(sp(s.dir / "nope"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------- 13

TEST(apply_fails_on_user_file_at_parent_path) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    write_file(src / "b.dds", "b");
    const fs::path farm = s.dir / "farm";

    const MergeResult r1 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r1, sp(farm)), sp(farm));

    const MergeResult r2 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
        {"Data/Sub", true, ""},
        {"Data/Sub/b.dds", false, sp(src / "b.dds")},
    });
    mol::Plan plan = mol::plan_farm(r2, sp(farm));
    CHECK_EQ(ops_str(plan), "Mkdir Data/Sub\nLink Data/Sub/b.dds -> " + sp(src / "b.dds") + "\n");

    // plan 之后、apply 之前，用户在父路径上放了真实文件
    write_file(farm / "Data" / "Sub", "user file");
    bool threw = false;
    try {
        mol::apply_farm(plan, sp(farm));
    } catch (const std::runtime_error& e) {
        threw = true;
        const std::string msg = e.what();
        CHECK(msg.find("Data/Sub") != std::string::npos);
        CHECK(msg.find("errno") != std::string::npos);
    }
    CHECK(threw);
    // 用户文件没被删，manifest 没丢原有记录
    CHECK(fs::is_regular_file(farm / "Data" / "Sub"));
    const Manifest j = read_manifest(farm);
    CHECK_EQ(j.created.size(), static_cast<std::size_t>(2));
    CHECK_EQ(j.created[0], "Data");
    CHECK_EQ(j.created[1], "Data/a.dds");
    // 原期望仍然收敛
    CHECK(mol::plan_farm(r1, sp(farm)).empty());

    // 用户撤掉文件后可以继续前进
    std::error_code ec;
    fs::remove(farm / "Data" / "Sub", ec);
    CHECK(!ec);
    mol::apply_farm(mol::plan_farm(r2, sp(farm)), sp(farm));
    CHECK(fs::is_symlink(farm / "Data" / "Sub" / "b.dds"));
    CHECK(mol::plan_farm(r2, sp(farm)).empty());
}

// ---------------------------------------------------------------- 14

TEST(conflicting_nonempty_dir_is_not_wiped) {
    Scratch s;
    const fs::path src = s.dir / "src";
    write_file(src / "a.dds", "a");
    write_file(src / "asfile.dds", "asfile");
    const fs::path farm = s.dir / "farm";

    const MergeResult r1 = make_result({
        {"Data", true, ""},
        {"Data/a.dds", false, sp(src / "a.dds")},
    });
    mol::apply_farm(mol::plan_farm(r1, sp(farm)), sp(farm));

    // 用户往我们创建的目录里塞了东西
    write_file(farm / "Data" / "user.txt", "mine");

    // 期望树把 Data 从目录改成文件：Data 在 manifest 里 → 计划 Remove + Link
    const MergeResult r2 = make_result({
        {"Data", false, sp(src / "asfile.dds")},
    });
    mol::Plan plan = mol::plan_farm(r2, sp(farm));
    CHECK_EQ(ops_str(plan),
             "Remove Data/a.dds\nRemove Data\nLink Data -> " + sp(src / "asfile.dds") + "\n");
    bool threw = false;
    try {
        mol::apply_farm(plan, sp(farm));
    } catch (const std::runtime_error& e) {
        threw = true;
        CHECK(std::string(e.what()).find("Remove 'Data'") != std::string::npos ||
              std::string(e.what()).find("Data") != std::string::npos);
    }
    CHECK(threw);
    // 用户内容完好；我们自己的 a.dds 已被清掉并从 manifest 注销，Data 仍在 manifest 里
    CHECK(fs::is_regular_file(farm / "Data" / "user.txt"));
    CHECK(!fs::exists(farm / "Data" / "a.dds"));
    const Manifest j = read_manifest(farm);
    CHECK_EQ(j.created.size(), static_cast<std::size_t>(1));
    CHECK_EQ(j.created[0], "Data");
    // 原期望仍可收敛：只需补回 a.dds，用户内容不动
    const mol::Plan again = mol::plan_farm(r1, sp(farm));
    CHECK_EQ(ops_str(again), "Link Data/a.dds -> " + sp(src / "a.dds") + "\n");
    mol::apply_farm(again, sp(farm));
    CHECK(mol::plan_farm(r1, sp(farm)).empty());
    CHECK(fs::is_regular_file(farm / "Data" / "user.txt"));
    CHECK(fs::is_symlink(farm / "Data" / "a.dds"));
}

}  // namespace
