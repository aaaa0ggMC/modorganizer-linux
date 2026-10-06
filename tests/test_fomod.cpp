#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/collection.hpp"
#include "mol/fomod.hpp"
#include "mol/xml.hpp"

namespace fs = std::filesystem;
using namespace mol;
using namespace mol::fomod;

namespace {
const char* kXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <moduleName>Test Mod</moduleName>
  <requiredInstallFiles>
    <file source="core\core.esp" destination="core.esp"/>
    <folder source="textures" destination="Textures" priority="1"/>
  </requiredInstallFiles>
  <installSteps order="Explicit">
    <installStep name="Main">
      <optionalFileGroups order="Explicit">
        <group name="Version" type="SelectExactlyOne">
          <plugins order="Explicit">
            <plugin name="Lite">
              <description>lite build</description>
              <files><file source="lite.esp" destination="mod.esp"/></files>
              <conditionFlags><flag name="edition">lite</flag></conditionFlags>
              <typeDescriptor><type name="Optional"/></typeDescriptor>
            </plugin>
            <plugin name="Full">
              <files><file source="full.esp" destination="mod.esp"/></files>
              <conditionFlags><flag name="edition">full</flag></conditionFlags>
              <typeDescriptor><type name="Recommended"/></typeDescriptor>
            </plugin>
          </plugins>
        </group>
        <group name="Extras" type="SelectAny">
          <plugins order="Explicit">
            <plugin name="Gold"><files><file source="gold.esp" destination="gold.esp" priority="5"/></files>
              <typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
            <plugin name="Locked"><typeDescriptor><type name="NotUsable"/></typeDescriptor></plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="FullOnly">
      <visible><flagDependency flag="edition" value="full"/></visible>
      <optionalFileGroups>
        <group name="Hi-res" type="SelectAtMostOne">
          <plugins order="Explicit">
            <plugin name="4K">
              <files><folder source="4k" destination=""/></files>
              <typeDescriptor><dependencyType>
                <defaultType name="NotUsable"/>
                <patterns><pattern>
                  <dependencies operator="And"><fileDependency file="Skyrim.esm" state="Active"/><flagDependency flag="edition" value="full"/></dependencies>
                  <type name="Recommended"/></pattern></patterns>
              </dependencyType></typeDescriptor>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
  <conditionalFileInstalls><patterns>
    <pattern><dependencies><flagDependency flag="edition" value="lite"/></dependencies>
      <files><file source="lite_patch.esp" destination="patch.esp"/></files></pattern>
  </patterns></conditionalFileInstalls>
</config>)";

Config cfg() { return parse_config(parse_xml(kXml)); }
Env env_with_skyrim() {
    Env e;
    e.file_state = [](std::string_view f) { return std::string(f) == "Skyrim.esm" ? "Active" : "Missing"; };
    return e;
}
std::string code_of(const std::function<void()>& f) {
    try { f(); } catch (const Error& e) { return e.code; }
    return "<no error>";
}
std::vector<std::string> sources(const Resolved& r) {
    std::vector<std::string> v;
    for (const auto& f : r.files) v.push_back(f.source);
    return v;
}
const PluginState* find_plugin(const Resolved& r, const char* step, const char* group, const char* plugin) {
    for (const auto& s : r.steps) if (s.name == step)
        for (const auto& g : s.groups) if (g.name == group)
            for (const auto& p : g.plugins) if (p.name == plugin) return &p;
    return nullptr;
}
}  // namespace

TEST(parse_structure) {
    const Config c = cfg();
    CHECK_EQ(c.module_name, std::string("Test Mod"));
    CHECK_EQ(c.required.size(), std::size_t{2});
    CHECK_EQ(c.steps.size(), std::size_t{2});
    CHECK(c.steps[1].has_visible);
    CHECK_EQ(c.steps[0].groups[0].plugins.size(), std::size_t{2});
    CHECK(c.steps[0].groups[0].type == GroupType::ExactlyOne);
    CHECK_EQ(c.conditional.size(), std::size_t{1});
}

TEST(defaults_follow_recommended_and_flags_drive_visibility) {
    const Config c = cfg();
    const Resolved r = resolve(c, {}, true, env_with_skyrim());
    // Version: Recommended=Full 被默认选中 → 设置 edition=full → 第二步可见
    CHECK(find_plugin(r, "Main", "Version", "Full")->selected);
    CHECK(!find_plugin(r, "Main", "Version", "Lite")->selected);
    CHECK(!find_plugin(r, "Main", "Extras", "Gold")->selected);
    CHECK(r.steps[1].visible);
    // 4K 的依赖模式命中 → Recommended → 默认选中
    CHECK(find_plugin(r, "FullOnly", "Hi-res", "4K")->type == PluginType::Recommended);
    CHECK(find_plugin(r, "FullOnly", "Hi-res", "4K")->selected);
    // 文件顺序：priority 0 的 required、full.esp、4k（priority 0），priority 1 的 textures 在后
    const auto s = sources(r);
    CHECK_EQ(s.front(), std::string("core\\core.esp"));
    CHECK_EQ(s.back(), std::string("textures"));
    CHECK(std::find(s.begin(), s.end(), std::string("lite_patch.esp")) == s.end());
}

TEST(explicit_choices_hide_steps_and_trigger_conditional_files) {
    const Config c = cfg();
    Choices ch;
    ch["Main"]["Version"] = {"Lite"};
    ch["Main"]["Extras"] = {"Gold"};
    const Resolved r = resolve(c, ch, false, env_with_skyrim());
    CHECK(!r.steps[1].visible);  // edition=lite → FullOnly 不可见
    const auto s = sources(r);
    CHECK(std::find(s.begin(), s.end(), std::string("lite.esp")) != s.end());
    CHECK(std::find(s.begin(), s.end(), std::string("lite_patch.esp")) != s.end());  // 条件安装
    CHECK(std::find(s.begin(), s.end(), std::string("full.esp")) == s.end());
    CHECK_EQ(s.back(), std::string("gold.esp"));  // priority 5 最后
    // 不可见步骤的组不需要给选择，也没有 4k 文件
    CHECK(std::find(s.begin(), s.end(), std::string("4k")) == s.end());
}

TEST(invalid_choices_are_rejected) {
    const Config c = cfg();
    Choices a;
    a["Main"]["Version"] = {"Lite", "Full"};  // ExactlyOne 选了两个
    a["Main"]["Extras"] = {};
    CHECK_EQ(code_of([&] { resolve(c, a, false, {}); }), std::string("invalid_argument"));
    Choices b;
    b["Main"]["Version"] = {"Lite"};
    b["Main"]["Extras"] = {"Locked"};  // NotUsable：与 MO2 一致，不能勾选 → 忽略（Vortex 记录的选择里会出现）
    {
        const auto r = resolve(c, b, false, {});
        for (const auto& g : r.steps[0].groups)
            for (const auto& p : g.plugins)
                if (p.name == "Locked") CHECK(!p.selected);
    }
    Choices d;
    d["Main"]["Version"] = {"Nope"};
    d["Main"]["Extras"] = {};
    CHECK_EQ(code_of([&] { resolve(c, d, false, {}); }), std::string("invalid_argument"));
    Choices e;  // 缺组，且不允许默认
    e["Main"]["Version"] = {"Lite"};
    CHECK_EQ(code_of([&] { resolve(c, e, false, {}); }), std::string("invalid_argument"));
}

TEST(xml_encodings_utf16_and_cp1252) {
    // UTF-16LE + BOM
    const std::string u8 = "<config><moduleName>A\xC3\xA9</moduleName></config>";
    std::string u16 = "\xFF\xFE";
    for (char ch : u8) { u16.push_back(ch); u16.push_back('\0'); }
    // é 需要 UTF-16 编码：C3 A9 是 UTF-8 字节，这里直接用码点 U+00E9 重新构造
    u16 = "\xFF\xFE";
    const std::u16string w = u"<config><moduleName>Aé</moduleName></config>";
    for (char16_t ch : w) { u16.push_back(static_cast<char>(ch & 0xFF)); u16.push_back(static_cast<char>(ch >> 8)); }
    CHECK_EQ(std::string(parse_xml(u16).child("moduleName")->text), u8.substr(u8.find('>', 8) + 1, 3));
    // windows-1252 声明
    const std::string x = "<?xml version=\"1.0\" encoding=\"windows-1252\"?><config><moduleName>caf\xE9</moduleName></config>";
    CHECK_EQ(std::string(parse_xml(x).child("moduleName")->text), std::string("caf\xC3\xA9"));
    CHECK_EQ(code_of([] { parse_xml("<config><a></config>"); }), std::string("invalid_argument"));
    CHECK_EQ(code_of([] { parse_config(parse_xml("<notconfig/>")); }), std::string("invalid_argument"));
}

TEST(install_files_case_insensitive_sources_backslashes_and_priority) {
    const fs::path t = fs::temp_directory_path() / ("mol_fomod_" + std::to_string(::getpid()));
    fs::remove_all(t);
    auto put = [&](const char* rel, const char* body) {
        fs::create_directories((t / "src" / rel).parent_path());
        std::ofstream(t / "src" / rel) << body;
    };
    put("Core/CORE.esp", "core");
    put("Textures/a/b.dds", "tex");
    put("full.esp", "full");
    put("4K/x.dds", "4k");
    fs::create_directories(t / "dst");
    const Config c = cfg();
    const Resolved r = resolve(c, {}, true, env_with_skyrim());
    std::vector<std::string> missing;
    const auto n = install_files(r, (t / "src").string(), (t / "dst").string(), &missing);
    CHECK(fs::exists(t / "dst/core.esp"));                 // core\core.esp 反斜杠 + 大小写不同
    CHECK(fs::exists(t / "dst/Textures/a/b.dds"));
    CHECK(fs::exists(t / "dst/mod.esp"));
    CHECK(fs::exists(t / "dst/x.dds"));                    // folder destination="" → mod 根
    CHECK_EQ(n, std::size_t{4});
    CHECK(missing.empty());
    // 越界路径
    Resolved bad;
    bad.files.push_back({"full.esp", "../escape.esp", false, 0});
    CHECK_EQ(code_of([&] { install_files(bad, (t / "src").string(), (t / "dst").string()); }), std::string("invalid_argument"));
    fs::remove_all(t);
}

TEST(choices_json_roundtrip) {
    Choices c;
    c["Main"]["Version"] = {"Full"};
    c["Main"]["Extras"] = {"Gold", "Silver"};
    const auto j = choices_to_json(c);
    CHECK(parse_choices_json(j) == c);
    CHECK_EQ(code_of([] { parse_choices_json("[]"); }), std::string("invalid_argument"));
    CHECK_EQ(code_of([] { parse_choices_json("{\"steps\":{\"a\":{\"b\":\"x\"}}}"); }), std::string("invalid_argument"));
}

namespace {
// 每一步都叫 "Installation"、组名都是 " "（真实集合里的 FOMOD 常这样写）；最后一步只有一个说明性的 "Finish Installation"
const char* kDupXml = R"(<config><moduleName>Dup</moduleName><installSteps order="Explicit">
<installStep name="Installation"><optionalFileGroups><group name=" " type="SelectExactlyOne"><plugins order="Explicit">
  <plugin name="High Poly"><files><file source="hp.esp" destination="x.esp"/></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
  <plugin name="Low Poly"><files><file source="lp.esp" destination="x.esp"/></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
</plugins></group></optionalFileGroups></installStep>
<installStep name="Installation"><optionalFileGroups><group name=" " type="SelectExactlyOne"><plugins order="Explicit">
  <plugin name="Finish Installation"><typeDescriptor><type name="Required"/></typeDescriptor></plugin>
</plugins></group></optionalFileGroups></installStep>
</installSteps></config>)";
const char* kTimingXml = R"(<config><moduleName>T</moduleName><installSteps order="Explicit">
<installStep name="Main"><optionalFileGroups><group name="Timing is Everything" type="SelectExactlyOne"><plugins order="Explicit">
  <plugin name="Default Timing"><files><file source="d.esp" destination="t.esp"/></files><typeDescriptor><type name="Recommended"/></typeDescriptor></plugin>
  <plugin name="Fast &amp; Loose"><files><file source="f.esp" destination="t.esp"/></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
</plugins></group></optionalFileGroups></installStep></installSteps></config>)";
std::string selected(const Resolved& r) {
    std::string s;
    for (const auto& st : r.steps)
        for (const auto& g : st.groups)
            for (const auto& p : g.plugins)
                if (p.selected) s += p.name + ";";
    return s;
}
}  // namespace

TEST(repeated_step_and_group_names_keep_their_own_choices) {
    const Config c = parse_config(parse_xml(kDupXml));
    // Vortex 清单：两个同名步骤按出现次序记录
    const Choices ch = mol::collection::choices_from_vortex(R"({"type":"fomod","options":[
        {"name":"Installation","groups":[{"name":" ","choices":[{"name":"Low Poly","idx":1}]}]},
        {"name":"Installation","groups":[{"name":" ","choices":[{"name":"Finish Installation","idx":0}]}]}]})");
    CHECK(ch.count("Installation") == 1 && ch.count(occurrence_key("Installation", 2)) == 1);
    const Resolved r = resolve(c, ch, false, {});  // 以前：两步的选择被合并 → "no plugin 'Finish Installation' in group ' '"
    CHECK_EQ(selected(r), std::string("Low Poly;Finish Installation;"));
}

TEST(names_match_ignoring_case_whitespace_and_html_entities) {
    const Config c = parse_config(parse_xml(kTimingXml));
    Choices ch;
    ch["main"]["timing is everything "] = {"Fast &amp; loose"};
    CHECK_EQ(selected(resolve(c, ch, false, {})), std::string("Fast & Loose;"));
}

TEST(lenient_mode_falls_back_to_defaults_with_notes) {
    const Config c = parse_config(parse_xml(kTimingXml));
    Choices gone;  // 清单里记录的选项在新版压缩包里没有了 → ExactlyOne 组一个都没选上
    gone["Main"]["Timing is Everything"] = {"Old Timing"};
    CHECK_EQ(code_of([&] { resolve(c, gone, false, {}); }), std::string("invalid_argument"));  // 严格模式照旧报错
    std::vector<std::string> notes;
    const Resolved r = resolve(c, gone, false, {}, &notes);
    CHECK_EQ(selected(r), std::string("Default Timing;"));
    CHECK_EQ(notes.size(), std::size_t{2});  // 忽略了不存在的插件 + 该组退回默认
    std::vector<std::string> n2;
    CHECK_EQ(selected(resolve(c, Choices{}, false, {}, &n2)), std::string("Default Timing;"));  // 没有记录的组：默认 + 说明
    CHECK_EQ(n2.size(), std::size_t{1});
}

TEST(xml_with_a_wrong_encoding_declaration_still_parses) {
    // 声明 UTF-16，内容其实是 UTF-8（"encoding specified in XML declaration is incorrect"）
    const std::string a = "<?xml version=\"1.0\" encoding=\"UTF-16\"?><config><moduleName>Hidden Hideouts</moduleName></config>";
    CHECK_EQ(std::string(parse_xml(a).child("moduleName")->text), std::string("Hidden Hideouts"));
    // UTF-16LE + BOM，却声明 UTF-8
    const std::u16string w = u"<?xml version=\"1.0\" encoding=\"UTF-8\"?><config><moduleName>Ré</moduleName></config>";
    std::string b = "\xFF\xFE";
    for (char16_t ch : w) { b.push_back(static_cast<char>(ch & 0xFF)); b.push_back(static_cast<char>(ch >> 8)); }
    CHECK_EQ(std::string(parse_xml(b).child("moduleName")->text), std::string("R\xC3\xA9"));
    // 声明 UTF-8，内容是 windows-1252
    const std::string c = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><config><moduleName>caf\xE9</moduleName></config>";
    CHECK_EQ(std::string(parse_xml(c).child("moduleName")->text), std::string("caf\xC3\xA9"));
    CHECK_EQ(code_of([] { parse_xml("<config><a></config>"); }), std::string("invalid_argument"));  // 真坏的 XML 照样报错
}
