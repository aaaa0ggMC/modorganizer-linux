#include "minitest.hpp"
#include "mol/loot.hpp"

using namespace mol;

namespace {
PluginRow row(const char* name, bool master = false, bool forced = false, std::vector<std::string> masters = {}) {
    PluginRow r;
    r.name = string(name);
    r.enabled = true;
    r.master = master;
    r.forced = forced;
    for (auto& m : masters) r.masters.push_back(string(m));
    return r;
}
std::string order(const PluginList& l) {
    std::string s;
    for (const auto& r : l.rows) { if (!s.empty()) s += ","; s += std::string(r.name); }
    return s;
}
const char* kYaml = R"(
groups:
  - name: early
  - name: default
    after: [early]
  - name: late
    after: [default]
plugins:
  - name: 'D.esp'
    group: early
  - name: 'A.esp'
    group: late
  - name: 'C.esp'
    after: ['B.esp']
  - name: 'Patch.*\.esp'
    after:
      - 'Base.esp'
      - name: 'Conditional.esp'
        condition: 'active("Whatever.esp")'
  - name: 'X.esp'
    after: ['Y.esp']
  - name: 'Y.esp'
    req: ['X.esp']
  - name: 'OnlyMessages.esp'
    msg: [{type: say, content: hi}]
)";
}  // namespace

TEST(parse_masterlist_groups_and_rules) {
    const auto ml = loot::parse_masterlist(kYaml);
    CHECK_EQ(ml.group_order.size(), std::size_t{3});
    CHECK(ml.group_rank.at("early") < ml.group_rank.at("default"));
    CHECK(ml.group_rank.at("default") < ml.group_rank.at("late"));
    bool regex_rule = false, msg_only = false;
    for (const auto& r : ml.rules) {
        if (r.name == "Patch.*\\.esp") { regex_rule = r.is_regex; CHECK_EQ(r.after.size(), std::size_t{1}); }  // 带 condition 的被忽略
        if (r.name == "OnlyMessages.esp") msg_only = true;
    }
    CHECK(regex_rule);
    CHECK(!msg_only);  // 没有分组也没有 after 的条目不保留
    bool threw = false;
    try { loot::parse_masterlist("- not a map"); } catch (const Error& e) { threw = e.code == "invalid_argument"; }
    CHECK(threw);
}

TEST(sort_applies_groups_after_rules_and_keeps_regions) {
    const auto ml = loot::parse_masterlist(kYaml);
    PluginList l;
    l.rows.push_back(row("Skyrim.esm", true, true));
    l.rows.push_back(row("Lib.esm", true));
    l.rows.push_back(row("A.esp"));
    l.rows.push_back(row("C.esp"));   // 要求排在 B 之后
    l.rows.push_back(row("B.esp"));
    l.rows.push_back(row("D.esp"));   // early 组
    const auto rep = loot::sort_with_masterlist(l, ml);
    // 强制 → ESM → ESP；ESP 区内：D(early) 先，其后默认组里 B 在 C 之前，A(late) 最后
    CHECK_EQ(order(l), std::string("Skyrim.esm,Lib.esm,D.esp,B.esp,C.esp,A.esp"));
    CHECK(rep.changed);
    CHECK(rep.rules_applied >= 1);
    CHECK_EQ(rep.grouped, std::size_t{2});
    // 幂等
    const auto again = loot::sort_with_masterlist(l, ml);
    CHECK(!again.changed);
    CHECK_EQ(order(l), std::string("Skyrim.esm,Lib.esm,D.esp,B.esp,C.esp,A.esp"));
}

TEST(regex_rules_masters_and_cycles) {
    const auto ml = loot::parse_masterlist(kYaml);
    PluginList l;
    l.rows.push_back(row("PatchOne.esp"));
    l.rows.push_back(row("Other.esp"));
    l.rows.push_back(row("Base.esp"));
    l.rows.push_back(row("Child.esp", false, false, {"Other.esp"}));   // master 依赖
    const auto rep = loot::sort_with_masterlist(l, ml);
    // 正则 Patch.*\.esp 要求在 Base.esp 之后；Child 要求在 Other 之后
    const std::string o = order(l);
    CHECK(o.find("Base.esp") < o.find("PatchOne.esp"));
    CHECK(o.find("Other.esp") < o.find("Child.esp"));
    (void)rep;
    // 成环（X after Y 且 Y req X）：不死循环，全部保留，并给出说明
    PluginList c;
    c.rows.push_back(row("X.esp"));
    c.rows.push_back(row("Y.esp"));
    const auto r2 = loot::sort_with_masterlist(c, ml);
    CHECK_EQ(c.rows.size(), std::size_t{2});
    CHECK(!r2.cycles.empty());
}
