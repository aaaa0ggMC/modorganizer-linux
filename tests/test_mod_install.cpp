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

TEST(fomod_choices_that_install_nothing_give_an_empty_mod) {
    if (!have_zip_tool()) return;
    Tmp t;
    Instance inst = make(t);
    put(t.dir / "game/Data/Skyrim.esm");
    inst.cfg.game_dir.assign((t.dir / "game").string());
    // 只有一个可多选的补丁组：一个都不选是合法的，结果没有任何文件（真实的 JK's Outskirts 补丁包就是这样）
    put(t.dir / "f/fomod/ModuleConfig.xml", R"(<config><moduleName>Patches</moduleName>
<installSteps><installStep name="S"><optionalFileGroups><group name="Misc" type="SelectAny"><plugins order="Explicit">
<plugin name="P"><files><file source="p.esp" destination="p.esp"/></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
</plugins></group></optionalFileGroups></installStep></installSteps></config>)");
    put(t.dir / "f/p.esp", "P");
    CHECK(zip_dir(t.dir / "f", t.dir / "patches.zip"));
    InstallOptions o;
    o.name = "Empty";
    o.fomod = FomodMode::Choices;
    o.choices["S"]["Misc"] = {};
    const auto r = install_archive(inst, (t.dir / "patches.zip").string(), o);
    CHECK(r.fomod);
    CHECK_EQ(r.files, std::size_t{0});
    CHECK(fs::is_directory(t.dir / "inst/mods/Empty"));
    CHECK(!fs::exists(t.dir / "inst/mods/Empty/p.esp"));
    CHECK_EQ(list_mods(inst).size(), std::size_t{1});
    // 不带 FOMOD 的空压缩包仍然是错误
    fs::create_directories(t.dir / "e/sub");
    put(t.dir / "e/sub/.keep", "");
    fs::remove(t.dir / "e/sub/.keep");
    CHECK(std::system(("cd '" + (t.dir / "e").string() + "' && zip -qr '" + (t.dir / "empty.zip").string() + "' . >/dev/null 2>&1").c_str()) == 0 || true);
    if (fs::exists(t.dir / "empty.zip"))
        CHECK_EQ(code_of([&] { install_archive(inst, (t.dir / "empty.zip").string(), "E2", false); }), std::string("invalid_argument"));
}

TEST(single_known_data_folder_is_not_stripped_as_a_wrapper) {
    if (!have_zip_tool()) return;
    Tmp t;
    const Instance inst = make(t);
    // 只有一个顶层目录 SKSE/（游戏数据目录）：必须原样保留
    put(t.dir / "a/SKSE/Plugins/x.dll");
    CHECK(zip_dir(t.dir / "a", t.dir / "skse_only.zip"));
    install_archive(inst, (t.dir / "skse_only.zip").string(), "SkseOnly");
    CHECK(fs::exists(t.dir / "inst/mods/SkseOnly/SKSE/Plugins/x.dll"));
    // 真正的包装目录（名字不是游戏数据目录）仍然被剥掉
    put(t.dir / "b/MyMod-1.2/meshes/m.nif");
    CHECK(zip_dir(t.dir / "b", t.dir / "wrapped.zip"));
    install_archive(inst, (t.dir / "wrapped.zip").string(), "Wrapped");
    CHECK(fs::exists(t.dir / "inst/mods/Wrapped/meshes/m.nif"));
    // 包装目录里再套一层游戏数据目录
    put(t.dir / "c/Pack/SKSE/Plugins/y.dll");
    CHECK(zip_dir(t.dir / "c", t.dir / "wrapped2.zip"));
    install_archive(inst, (t.dir / "wrapped2.zip").string(), "Wrapped2");
    CHECK(fs::exists(t.dir / "inst/mods/Wrapped2/SKSE/Plugins/y.dll"));
}

TEST(fomod_images_are_extracted_case_insensitively) {
    if (!have_zip_tool() || std::system("command -v 7z >/dev/null 2>&1 || command -v 7zz >/dev/null 2>&1") != 0) return;
    Tmp t;
    const Instance inst = make(t);
    // 外面包一层目录；图片的真实大小写与配置里的不同
    put(t.dir / "f/Wrap/fomod/ModuleConfig.xml", "<config/>");
    put(t.dir / "f/Wrap/Images/Header.PNG", "png-a");
    put(t.dir / "f/Wrap/other/sub/Header.png", "png-b");
    put(t.dir / "f/Wrap/textures/big.dds", "not an image we want");
    CHECK(zip_dir(t.dir / "f", t.dir / "fomod.zip"));
    const std::vector<std::string> imgs{"images\\header.png", "other/sub/header.png", "missing.png", "..\\evil.png"};
    const auto got = extract_fomod_images(inst, (t.dir / "fomod.zip").string(), imgs, (t.dir / "imgs").string());
    CHECK_EQ(got.size(), std::size_t{2});
    auto body = [](const std::string& p) { std::ifstream in(p); std::string s; std::getline(in, s); return s; };
    CHECK(got.count("images\\header.png") && body(got.at("images\\header.png")) == "png-a");
    CHECK(got.count("other/sub/header.png") && body(got.at("other/sub/header.png")) == "png-b");
    CHECK(!fs::exists(t.dir / "imgs/big.dds"));
    // 只解了图片：临时目录已清理
    std::size_t leftovers = 0;
    for (const auto& e : fs::directory_iterator(t.dir / "inst/downloads")) leftovers += e.path().filename().string().starts_with(".mol-fomod") ? 1 : 0;
    CHECK_EQ(leftovers, std::size_t{0});
}

TEST(very_long_mod_names_are_shortened_stably) {
    // 真实集合里的名字：150–250 字符，常带「…」「–」这类多字节字符，再加集合的 " [tag]" 后缀 → 超过 255 字节
    std::string longname = "Draugrs - My patches - SE by Xtudo - Diverse Dragon Priests";
    while (longname.size() < 300) longname += " \xE2\x80\x94 Xavbio Dragon Priests \xE2\x80\xA6";
    const std::string a = sanitize_mod_name(longname);
    CHECK(a.size() <= 100);
    CHECK_EQ(a, sanitize_mod_name(longname));                    // 稳定：重跑得到同一个目录名
    CHECK(a != sanitize_mod_name(longname + " [tag]"));          // 不同的长名不撞
    CHECK(a.rfind("Draugrs - My patches", 0) == 0);              // 前缀可读
    for (std::size_t i = 0; i < a.size(); ++i)                   // 截在 UTF-8 字符边界上：结尾不是半个字符
        if (static_cast<unsigned char>(a[i]) >= 0xC0) CHECK(i + 1 < a.size());
    CHECK_EQ(sanitize_mod_name("Short Name"), std::string("Short Name"));
    if (!have_zip_tool()) return;
    Tmp t;
    const Instance inst = make(t);
    put(t.dir / "src/meshes/a.nif");
    CHECK(zip_dir(t.dir / "src", t.dir / "m.zip"));
    InstallOptions opt;
    opt.name = longname;
    const auto res = install_archive(inst, (t.dir / "m.zip").string(), opt);  // 以前：rename → ENAMETOOLONG
    CHECK_EQ(std::string(res.name), a);
    CHECK(fs::exists(t.dir / "inst/mods" / a / "meshes/a.nif"));
    CHECK(mod_name_taken(inst, longname, "Default"));
}

TEST(data_folder_next_to_readme_files_is_the_mod_root) {
    if (!have_zip_tool()) return;
    Tmp t;
    const Instance inst = make(t);
    // Data/ + 说明文档：Data 才是根（真实例子：Interesting NPCs 4.5 → 4.53 Hotfix）
    put(t.dir / "a/Data/3DNPC.esp");
    put(t.dir / "a/Data/Scripts/x.pex");
    put(t.dir / "a/Patch Notes.txt");
    CHECK(zip_dir(t.dir / "a", t.dir / "a.zip"));
    install_archive(inst, (t.dir / "a.zip").string(), "Hotfix");
    CHECK(fs::exists(t.dir / "inst/mods/Hotfix/3DNPC.esp"));
    CHECK(fs::exists(t.dir / "inst/mods/Hotfix/Scripts/x.pex"));
    CHECK(fs::exists(t.dir / "inst/mods/Hotfix/Patch Notes.txt"));
    CHECK(!fs::exists(t.dir / "inst/mods/Hotfix/Data"));
    // 顶层已经是游戏数据（有 meshes/）时，旁边的 Data 目录不动
    put(t.dir / "b/meshes/m.nif");
    put(t.dir / "b/Data/odd.txt");
    CHECK(zip_dir(t.dir / "b", t.dir / "b.zip"));
    install_archive(inst, (t.dir / "b.zip").string(), "Mixed");
    CHECK(fs::exists(t.dir / "inst/mods/Mixed/meshes/m.nif"));
    CHECK(fs::exists(t.dir / "inst/mods/Mixed/Data/odd.txt"));
}

TEST(archive_living_entirely_inside_fomod_is_still_a_fomod) {
    if (!have_zip_tool()) return;
    Tmp t;
    const Instance inst = make(t);
    // 真实例子：Unofficial Lux Patchhub——所有东西都在 Fomod/ 下，源路径写 fomod\...
    std::string xml = R"(<?xml version="1.0"?><config><installSteps order="Explicit"><installStep name="S"><optionalFileGroups>
<group name="G" type="SelectAny"><plugins order="Explicit">
<plugin name="P"><files><file source="fomod\patches\p.esp" destination="p.esp"/></files><typeDescriptor><type name="Recommended"/></typeDescriptor></plugin>
</plugins></group></optionalFileGroups></installStep></installSteps></config>)";
    fs::create_directories(t.dir / "a/Fomod");
    std::ofstream(t.dir / "a/Fomod/ModuleConfig.xml") << xml;
    put(t.dir / "a/Fomod/patches/p.esp", "P");
    put(t.dir / "a/Fomod/patches/unused.esp", "U");
    CHECK(zip_dir(t.dir / "a", t.dir / "a.zip"));
    InstallOptions o;
    o.name = "Hub";
    o.fomod = FomodMode::Defaults;
    const auto r = install_archive(inst, (t.dir / "a.zip").string(), o);
    CHECK(r.fomod);
    CHECK(fs::exists(t.dir / "inst/mods/Hub/p.esp"));
    CHECK(!fs::exists(t.dir / "inst/mods/Hub/patches"));
}

TEST(backslash_paths_from_windows_zips_become_directories) {
    if (!have_zip_tool()) return;
    Tmp t;
    const Instance inst = make(t);
    // 真实例子：Skyrim Priority SE AE 3.4.0 的 zip 里条目名是 "Data\SKSE\Plugins\PriorityMod.dll"
    put(t.dir / "a" / "Data\\SKSE\\Plugins\\PriorityMod.dll", "D");
    put(t.dir / "a" / "Data\\SKSE\\Plugins\\PriorityMod.toml", "T");
    CHECK(zip_dir(t.dir / "a", t.dir / "a.zip"));
    install_archive(inst, (t.dir / "a.zip").string(), "Priority");
    CHECK(fs::exists(t.dir / "inst/mods/Priority/SKSE/Plugins/PriorityMod.dll"));
    CHECK(fs::exists(t.dir / "inst/mods/Priority/SKSE/Plugins/PriorityMod.toml"));
    // 拆出来越界的拒绝
    put(t.dir / "b" / "..\\..\\evil.dll", "E");
    CHECK(zip_dir(t.dir / "b", t.dir / "b.zip"));
    CHECK_EQ(code_of([&] { install_archive(inst, (t.dir / "b.zip").string(), "Evil"); }), std::string("invalid_argument"));
    CHECK(!fs::exists(t.dir / "inst/evil.dll"));
}
