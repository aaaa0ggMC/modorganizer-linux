#include <unistd.h>

#include <cstdlib>
#include <functional>
#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/instance.hpp"
#include "mol/mod_install.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_mi_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void put(const fs::path& p, const char* body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}
bool have_zip_tool() { return std::system("command -v zip >/dev/null 2>&1") == 0; }
// 在 src 目录里把 entries 打成 zip（相对路径）
bool zip_dir(const fs::path& src, const fs::path& out) {
    const std::string cmd = "cd '" + src.string() + "' && zip -qr '" + out.string() + "' . >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}
Instance make(const Tmp& t) {
    Instance i;
    i.root.assign((t.dir / "inst").string());
    i.mods_dir.assign((t.dir / "inst/mods").string());
    i.profiles_dir.assign((t.dir / "inst/profiles").string());
    i.overwrite_dir.assign((t.dir / "inst/overwrite").string());
    i.downloads_dir.assign((t.dir / "inst/downloads").string());
    i.cfg.profile.assign("Default");
    fs::create_directories(t.dir / "inst/profiles/Default");
    fs::create_directories(t.dir / "inst/mods");
    return i;
}
std::string code_of(const std::function<void()>& f) {
    try { f(); } catch (const Error& e) { return e.code; }
    return "<no error>";
}
}  // namespace

TEST(install_layouts_root_data_and_wrapper) {
    if (!have_zip_tool()) return;  // 没有 zip 就跳过（环境相关）
    Tmp t;
    const Instance inst = make(t);

    // 根目录型：顶层有 exe；外面还套了一层目录
    put(t.dir / "a/pack-1.0/loader.exe");
    put(t.dir / "a/pack-1.0/Data/Scripts/x.pex");
    CHECK(zip_dir(t.dir / "a", t.dir / "root.zip"));
    const auto r1 = install_archive(inst, (t.dir / "root.zip").string(), "RootMod");
    CHECK(r1.root);
    CHECK(fs::exists(t.dir / "inst/mods/RootMod/loader.exe"));
    CHECK(fs::exists(t.dir / "inst/mods/RootMod/Data/Scripts/x.pex"));

    // 普通 mod：只有一个 Data 目录 → 去掉 Data
    put(t.dir / "b/Data/meshes/m.nif");
    CHECK(zip_dir(t.dir / "b", t.dir / "data.zip"));
    const auto r2 = install_archive(inst, (t.dir / "data.zip").string(), "DataMod");
    CHECK(!r2.root);
    CHECK(fs::exists(t.dir / "inst/mods/DataMod/meshes/m.nif"));
    CHECK(!fs::exists(t.dir / "inst/mods/DataMod/Data"));

    // 已有同名 → 拒绝
    CHECK_EQ(code_of([&] { install_archive(inst, (t.dir / "data.zip").string(), "datamod"); }), std::string("invalid_argument"));

    // modlist：后装的在最高优先级（list_mods 低→高）；root 标记被读到
    const auto mods = list_mods(inst);
    CHECK_EQ(mods.size(), std::size_t{2});
    CHECK_EQ(std::string(mods[0].name), std::string("RootMod"));
    CHECK(mods[0].root);
    CHECK(mods[0].enabled);
    CHECK(!mods[1].root);
    CHECK(!fs::exists(t.dir / "inst/mods/.mol-extract-0"));
}

TEST(install_rejects_symlinks_and_missing_archive_and_leaves_no_debris) {
    if (!have_zip_tool()) return;
    Tmp t;
    const Instance inst = make(t);
    fs::create_directories(t.dir / "c");
    fs::create_symlink("/etc/passwd", t.dir / "c/evil");
    put(t.dir / "c/ok.txt");
    CHECK(std::system(("cd '" + (t.dir / "c").string() + "' && zip -qry '" + (t.dir / "evil.zip").string() + "' . >/dev/null 2>&1").c_str()) == 0);
    CHECK_EQ(code_of([&] { install_archive(inst, (t.dir / "evil.zip").string(), "Evil"); }), std::string("invalid_argument"));
    CHECK(!fs::exists(t.dir / "inst/mods/Evil"));
    CHECK_EQ(list_mods(inst).size(), std::size_t{0});
    for (const auto& e : fs::directory_iterator(t.dir / "inst/mods")) { (void)e; CHECK(false); }  // 没有残留
    CHECK_EQ(code_of([&] { install_archive(inst, (t.dir / "nope.zip").string()); }), std::string("invalid_argument"));
}

TEST(root_mod_maps_to_farm_root_and_meta_ini_is_not_linked) {
    Tmp t;
    Instance inst = make(t);
    const fs::path game = t.dir / "game";
    put(game / "SkyrimSE.exe");
    put(game / "Data/Skyrim.esm");
    inst.cfg.game_dir.assign(game.string());
    inst.farm_path.assign((t.dir / "inst/farm").string());
    put(t.dir / "inst/mods/Root/skse_loader.exe");
    put(t.dir / "inst/mods/Root/Data/Scripts/a.pex");
    put(t.dir / "inst/mods/Plain/Textures/t.dds");
    put(t.dir / "inst/mods/Plain/meta.ini", "[General]\nmodid=1\n");
    mark_mod_root((t.dir / "inst/mods/Root").string(), true);
    CHECK(fs::exists(t.dir / "inst/mods/Root/meta.ini"));
    put(t.dir / "inst/profiles/Default/modlist.txt", "+Plain\n+Root\n");

    const auto model = build_farm_model(inst);
    bool loader = false, script = false, tex = false, meta = false;
    for (const auto& e : model.merged.entries) {
        const std::string p(e.path);
        if (p == "skse_loader.exe") loader = true;
        if (p == "Data/Scripts/a.pex") script = true;
        if (p == "Data/Textures/t.dds") tex = true;
        if (p.find("meta.ini") != std::string::npos) meta = true;
    }
    CHECK(loader);
    CHECK(script);
    CHECK(tex);
    CHECK(!meta);
}

TEST(mark_mod_root_preserves_existing_meta_ini) {
    Tmp t;
    put(t.dir / "m/meta.ini", "[General]\nmodid=7\nversion=1.2\n\n[installedFiles]\n1\\modid=7\n");
    mark_mod_root((t.dir / "m").string(), true);
    std::ifstream in(t.dir / "m/meta.ini");
    const std::string s((std::istreambuf_iterator<char>(in)), {});
    CHECK(s.find("modid=7") != std::string::npos);
    CHECK(s.find("version=1.2") != std::string::npos);
    CHECK(s.find("1\\modid=7") != std::string::npos);
    CHECK(s.find("mol_root=true") != std::string::npos);
    mark_mod_root((t.dir / "m").string(), false);  // 幂等：只改值，不重复
    std::ifstream in2(t.dir / "m/meta.ini");
    const std::string s2((std::istreambuf_iterator<char>(in2)), {});
    CHECK(s2.find("mol_root=false") != std::string::npos);
    CHECK_EQ(s2.find("mol_root=true"), std::string::npos);
}

TEST(install_fomod_modes) {
    if (!have_zip_tool()) return;
    Tmp t;
    Instance inst = make(t);
    const fs::path game = t.dir / "game";
    put(game / "Data/Skyrim.esm");
    inst.cfg.game_dir.assign(game.string());
    fs::create_directories(t.dir / "f/fomod");
    {
        std::ofstream(t.dir / "f/fomod/ModuleConfig.xml") << R"(<config><moduleName>M</moduleName>
<requiredInstallFiles><file source="req.txt" destination="req.txt"/></requiredInstallFiles>
<installSteps><installStep name="S"><optionalFileGroups><group name="G" type="SelectExactlyOne"><plugins order="Explicit">
<plugin name="A"><files><file source="a.txt" destination="chosen.txt"/></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
<plugin name="B"><files><file source="b.txt" destination="chosen.txt"/></files><typeDescriptor><type name="Recommended"/></typeDescriptor></plugin>
</plugins></group></optionalFileGroups></installStep></installSteps></config>)";
    }
    put(t.dir / "f/req.txt", "req");
    put(t.dir / "f/a.txt", "A");
    put(t.dir / "f/b.txt", "B");
    put(t.dir / "f/unselected_extra.txt", "never");
    CHECK(zip_dir(t.dir / "f", t.dir / "fomod.zip"));
    auto slurp = [](const fs::path& p) { std::ifstream in(p); return std::string((std::istreambuf_iterator<char>(in)), {}); };

    // Unset → 必须明确选择
    CHECK_EQ(code_of([&] { InstallOptions o; o.name = "X"; install_archive(inst, (t.dir / "fomod.zip").string(), o); }),
             std::string("fomod_choices_required"));
    CHECK(!fs::exists(t.dir / "inst/mods/X"));
    CHECK_EQ(list_mods(inst).size(), std::size_t{0});

    // 只读检查能读到配置
    const auto cfg = read_archive_fomod(inst, (t.dir / "fomod.zip").string());
    CHECK(cfg.has_value());
    CHECK_EQ(cfg->module_name, std::string("M"));
    if (fs::exists(t.dir / "inst/downloads")) for (const auto& e : fs::directory_iterator(t.dir / "inst/downloads")) { (void)e; CHECK(false); }  // 临时目录已清理

    // Defaults → B（Recommended）；未被 FOMOD 选中的文件不会出现
    InstallOptions d;
    d.name = "Def";
    d.fomod = FomodMode::Defaults;
    const auto r1 = install_archive(inst, (t.dir / "fomod.zip").string(), d);
    CHECK(r1.fomod);
    CHECK_EQ(slurp(t.dir / "inst/mods/Def/chosen.txt"), std::string("B"));
    CHECK(fs::exists(t.dir / "inst/mods/Def/req.txt"));
    CHECK(!fs::exists(t.dir / "inst/mods/Def/unselected_extra.txt"));
    CHECK(!fs::exists(t.dir / "inst/mods/Def/fomod"));

    // Choices → A
    InstallOptions c;
    c.name = "Cho";
    c.fomod = FomodMode::Choices;
    c.choices["S"]["G"] = {"A"};
    install_archive(inst, (t.dir / "fomod.zip").string(), c);
    CHECK_EQ(slurp(t.dir / "inst/mods/Cho/chosen.txt"), std::string("A"));

    // Choices 缺组且不允许默认 → 报错，不留痕迹
    InstallOptions m;
    m.name = "Miss";
    m.fomod = FomodMode::Choices;
    CHECK_EQ(code_of([&] { install_archive(inst, (t.dir / "fomod.zip").string(), m); }), std::string("invalid_argument"));
    CHECK(!fs::exists(t.dir / "inst/mods/Miss"));

    // Raw → 原样安装，包括 fomod 目录与未选文件
    install_archive(inst, (t.dir / "fomod.zip").string(), "RawMod", false);
    CHECK(fs::exists(t.dir / "inst/mods/RawMod/fomod/ModuleConfig.xml"));
    CHECK(fs::exists(t.dir / "inst/mods/RawMod/unselected_extra.txt"));
}
