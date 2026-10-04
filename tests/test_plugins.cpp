#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/plugins.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_pl_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};

// 造一个最小的 TES4 插件：flags + 若干 masters
void make_plugin(const fs::path& p, std::uint32_t flags, const std::vector<std::string>& masters) {
    std::string data;
    auto sub = [&](const char* type, const std::string& body) {
        data.append(type, 4);
        data.push_back(static_cast<char>(body.size() & 0xFF));
        data.push_back(static_cast<char>(body.size() >> 8));
        data += body;
    };
    sub("HEDR", std::string(12, '\0'));
    for (const auto& m : masters) {
        sub("MAST", m + std::string(1, '\0'));
        sub("DATA", std::string(8, '\0'));
    }
    std::string rec = "TES4";
    auto le32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) rec.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); };
    le32(static_cast<std::uint32_t>(data.size()));
    le32(flags);
    le32(0); le32(0);
    rec.push_back(44); rec.push_back(0); rec.push_back(0); rec.push_back(0);
    rec += data;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << rec;
}
void put(const fs::path& p, const char* body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}
std::string code_of(const std::function<void()>& f) {
    try { f(); } catch (const Error& e) { return e.code; }
    return "<no error>";
}
std::vector<std::string> names(const PluginList& l) {
    std::vector<std::string> v;
    for (const auto& r : l.rows) v.emplace_back(r.name);
    return v;
}
std::string nm(const PluginList& l, std::size_t i) { return std::string(l.rows[i].name); }
Instance make(const Tmp& t) {
    Instance i;
    i.root.assign((t.dir / "inst").string());
    i.mods_dir.assign((t.dir / "inst/mods").string());
    i.profiles_dir.assign((t.dir / "inst/profiles").string());
    i.overwrite_dir.assign((t.dir / "inst/overwrite").string());
    i.farm_path.assign((t.dir / "inst/farm").string());
    i.cfg.game_dir.assign((t.dir / "game").string());
    i.cfg.profile.assign("Default");
    fs::create_directories(t.dir / "inst/profiles/Default");
    return i;
}
}  // namespace

TEST(header_flags_and_masters) {
    Tmp t;
    make_plugin(t.dir / "a.esp", 0x1 | 0x200, {"Skyrim.esm", "Other.esp"});
    const auto h = read_plugin_header((t.dir / "a.esp").string());
    CHECK(h.master_flag);
    CHECK(h.light_flag);
    CHECK_EQ(h.masters.size(), std::size_t{2});
    CHECK_EQ(std::string(h.masters[1]), std::string("Other.esp"));
    put(t.dir / "bad.esp", "not a plugin at all, definitely not TES4 data");
    CHECK_EQ(code_of([&] { read_plugin_header((t.dir / "bad.esp").string()); }), std::string("invalid_argument"));
    CHECK_EQ(code_of([&] { read_plugin_header((t.dir / "nope.esp").string()); }), std::string("io_error"));
}

TEST(load_discovers_orders_and_enforces_regions) {
    Tmp t;
    const Instance inst = make(t);
    make_plugin(t.dir / "game/Data/Skyrim.esm", 0x1, {});
    make_plugin(t.dir / "game/Data/Update.esm", 0x1, {"Skyrim.esm"});
    put(t.dir / "game/SkyrimSE.exe", "x");
    make_plugin(t.dir / "inst/mods/M/Data/ModB.esp", 0, {"Skyrim.esm", "ModA.esp"});
    make_plugin(t.dir / "inst/mods/M/Data/ModA.esp", 0, {"Skyrim.esm"});
    make_plugin(t.dir / "inst/mods/M/Data/Lib.esm", 0x1, {"Skyrim.esm"});
    // mod 目录结构：mods/M/ 直接是 Data 内容
    fs::rename(t.dir / "inst/mods/M/Data/ModB.esp", t.dir / "inst/mods/M/ModB.esp");
    fs::rename(t.dir / "inst/mods/M/Data/ModA.esp", t.dir / "inst/mods/M/ModA.esp");
    fs::rename(t.dir / "inst/mods/M/Data/Lib.esm", t.dir / "inst/mods/M/Lib.esm");
    put(t.dir / "inst/profiles/Default/modlist.txt", "+M\n");
    // profile 里已有的顺序故意违反区域规则，并包含一个磁盘上没有的名字
    put(t.dir / "inst/profiles/Default/plugins.txt", "ModB.esp\n*Gone.esp\n*Lib.esm\n");

    const std::vector<string> forced{string("Skyrim.esm"), string("Update.esm")};
    auto l = load_plugins(inst, forced);
    // 强制 → ESM 标志 → 其它；Gone 被丢弃；ModA 是新发现的，追加在 ModB 之后（同区内）
    CHECK_EQ(nm(l, 0), std::string("Skyrim.esm"));
    CHECK_EQ(nm(l, 1), std::string("Update.esm"));
    CHECK_EQ(nm(l, 2), std::string("Lib.esm"));
    CHECK_EQ(l.rows.size(), std::size_t{5});
    CHECK_EQ(nm(l, 3), std::string("ModB.esp"));
    CHECK_EQ(nm(l, 4), std::string("ModA.esp"));
    CHECK(l.rows[0].forced && l.rows[0].enabled);
    CHECK(!l.rows[3].enabled);   // plugins.txt 里无 '*' → 禁用
    CHECK(l.rows[4].enabled);    // 新发现 → 启用
    CHECK_EQ(std::string(l.rows[3].source), std::string("M"));
    CHECK(l.rows[2].master);

    // 依赖检查：ModB 被禁用所以不查；启用后 ModA 在它后面 → after
    CHECK(check_masters(l).empty());
    CHECK(plugin_set_enabled(l, "modb.esp", true));
    const auto issues = check_masters(l);
    CHECK_EQ(issues.size(), std::size_t{1});
    CHECK_EQ(std::string(issues[0].kind), std::string("after"));
    CHECK(plugin_sort_by_masters(l));
    CHECK_EQ(nm(l, 3), std::string("ModA.esp"));
    CHECK_EQ(nm(l, 4), std::string("ModB.esp"));
    CHECK(check_masters(l).empty());
    CHECK(!plugin_sort_by_masters(l));  // 幂等

    // 禁用 master → disabled；缺失 master → missing
    CHECK(plugin_set_enabled(l, "ModA.esp", false));
    CHECK_EQ(std::string(check_masters(l)[0].kind), std::string("disabled"));
    // 强制插件不能禁用；不存在的插件报 mod_not_found
    CHECK_EQ(code_of([&] { plugin_set_enabled(l, "Skyrim.esm", false); }), std::string("invalid_argument"));
    CHECK_EQ(code_of([&] { plugin_set_enabled(l, "Nope.esp", true); }), std::string("mod_not_found"));

    // move：不能越过区域边界（移到 0 会被 normalize 修正回 ESM 区之后）
    plugin_move(l, "ModB.esp", 0);
    CHECK_EQ(nm(l, 3), std::string("ModB.esp"));  // 移到非 ESM 区的最前（被区域规则拦在 ESM 之后）
    CHECK_EQ(nm(l, 4), std::string("ModA.esp"));
    CHECK_EQ(nm(l, 2), std::string("Lib.esm"));

    // 保存后重新读取，状态一致
    save_plugins(inst, l);
    const auto l2 = load_plugins(inst, forced);
    CHECK(names(l2) == names(l));
    for (std::size_t i = 0; i < l.rows.size(); ++i) CHECK_EQ(l2.rows[i].enabled, l.rows[i].enabled);
}

TEST(missing_master_is_reported) {
    Tmp t;
    const Instance inst = make(t);
    make_plugin(t.dir / "game/Data/Skyrim.esm", 0x1, {});
    put(t.dir / "game/SkyrimSE.exe", "x");
    make_plugin(t.dir / "inst/mods/M/Orphan.esp", 0, {"Skyrim.esm", "Absent.esm"});
    put(t.dir / "inst/profiles/Default/modlist.txt", "+M\n");
    const std::vector<string> forced{string("Skyrim.esm")};
    const auto l = load_plugins(inst, forced);
    const auto issues = check_masters(l);
    CHECK_EQ(issues.size(), std::size_t{1});
    CHECK_EQ(std::string(issues[0].master), std::string("Absent.esm"));
    CHECK_EQ(std::string(issues[0].kind), std::string("missing"));
}
