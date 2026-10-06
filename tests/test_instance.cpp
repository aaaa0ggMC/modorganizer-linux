#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/instance.hpp"
#include "mol/mo2fmt.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_inst_" + std::to_string(::getpid()) + "_" + std::to_string(counter()++));
        CHECK(dir.string().rfind("/tmp/", 0) == 0);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
    static int& counter() { static int c = 0; return c; }
};
void touch(const fs::path& p, const char* body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}
}  // namespace

// 造一个实例：游戏目录 + 两个 mod（有大小写冲突）+ profile
static void make_fixture(const Tmp& t, fs::path& inst, fs::path& game) {
    inst = t.dir / "inst";
    game = t.dir / "game";
    touch(game / "SkyrimSE.exe");
    touch(game / "Data" / "Skyrim.esm");
    touch(game / "Data" / "Textures" / "base.dds");
    const std::string root_s = inst.string(), game_s = game.string(), pfx_s = (t.dir / "pfx").string();
    InitOptions o;  // 字段是 string_view：被引用的字符串必须存活
    o.root = root_s;
    o.game_dir = game_s;
    o.prefix = pfx_s;
    CHECK(init_instance(o));
    touch(inst / "mods" / "ModA" / "textures" / "a.dds");
    touch(inst / "mods" / "ModA" / "Textures" / "base.dds", "A");
    touch(inst / "mods" / "ModB" / "TEXTURES" / "base.dds", "B");
    touch(inst / "mods" / "ModB" / "b.esp");
    // modlist：文件第一条为最高优先级（ModB > ModA）
    touch(inst / "profiles" / "Default" / "modlist.txt", "+ModB\n-ModOff\n+ModA\n");
}

TEST(init_is_idempotent_and_preserves) {
    Tmp t;
    InitOptions o;
    std::string root = (t.dir / "i").string();
    o.root = root;
    o.game_dir = "/g";
    CHECK(init_instance(o));
    CHECK(!init_instance(o));  // 第二次无变化
    InitOptions o2;
    o2.root = root;
    o2.proton_path = "/p";
    o2.game = ""; o2.prefix_user = ""; o2.profile = ""; o2.runner_kind = "";  // 只改 proton_path
    CHECK(init_instance(o2));
    auto inst = load_instance(root);
    CHECK_EQ(std::string(inst.cfg.game_dir), std::string("/g"));  // 保留
    CHECK_EQ(std::string(inst.cfg.proton_path), std::string("/p"));
}

TEST(load_instance_errors_and_defaults) {
    Tmp t;
    bool threw = false;
    try { load_instance((t.dir / "nope").string()); } catch (const Error& e) { threw = e.code == "instance_not_found"; }
    CHECK(threw);
    fs::create_directories(t.dir / "empty");
    threw = false;
    try { load_instance((t.dir / "empty").string()); } catch (const Error& e) { threw = e.code == "instance_not_found"; }
    CHECK(threw);
    // 仅有 ModOrganizer.ini：推导 gamePath / selected_profile
    fs::create_directories(t.dir / "mo");
    std::ofstream(t.dir / "mo" / "ModOrganizer.ini")
        << "[General]\ngameName=Skyrim Special Edition\ngamePath=@ByteArray(Z:\\\\opt\\\\skyrim)\nselected_profile=@ByteArray(Mine)\n";
    auto inst = load_instance((t.dir / "mo").string());
    CHECK_EQ(std::string(inst.cfg.game), std::string("skyrimse"));
    CHECK_EQ(std::string(inst.cfg.game_dir), std::string("/opt/skyrim"));
    CHECK_EQ(std::string(inst.cfg.profile), std::string("Mine"));
    CHECK_EQ(std::string(inst.mods_dir), (t.dir / "mo" / "mods").string());
    auto ov = load_instance((t.dir / "mo").string(), "Other");
    CHECK_EQ(std::string(ov.cfg.profile), std::string("Other"));
}

TEST(mods_list_enable_move) {
    Tmp t; fs::path inst_p, game;
    make_fixture(t, inst_p, game);
    auto inst = load_instance(inst_p.string());
    auto mods = list_mods(inst);
    CHECK_EQ(mods.size(), std::size_t(3));
    CHECK_EQ(std::string(mods[0].name), std::string("ModA"));   // 低→高
    CHECK_EQ(std::string(mods[2].name), std::string("ModB"));
    CHECK(mods[0].exists);
    CHECK(!mods[1].exists);  // ModOff 无目录
    CHECK(!set_mod_enabled(inst, "moda", true));          // 已启用 → 无变化，大小写不敏感
    CHECK(set_mod_enabled(inst, "ModOff", true));
    CHECK(!set_mod_enabled(inst, "ModOff", true));        // 幂等
    CHECK(move_mod(inst, "ModB", 0));
    CHECK(!move_mod(inst, "ModB", 0));
    CHECK_EQ(std::string(list_mods(inst)[0].name), std::string("ModB"));
    bool threw = false;
    try { set_mod_enabled(inst, "Nope", true); } catch (const Error& e) { threw = e.code == "mod_not_found"; }
    CHECK(threw);
    threw = false;
    try { set_mod_enabled(inst, "ModA", true, "NoProfile"); } catch (const Error& e) { threw = e.code == "profile_not_found"; }
    CHECK(threw);
}

TEST(farm_end_to_end) {
    Tmp t; fs::path inst_p, game;
    make_fixture(t, inst_p, game);
    auto inst = load_instance(inst_p.string());
    auto model = build_farm_model(inst);
    // 层：game + ModA + ModB（ModOff 禁用；无 overwrite 内容但目录存在 → 有该层）
    CHECK_EQ(std::string(model.layer_names[0]), std::string("<game>"));
    CHECK_EQ(std::string(model.layer_names[1]), std::string("ModA"));
    CHECK_EQ(std::string(model.layer_names[2]), std::string("ModB"));
    auto plan = plan_instance(inst, model);
    CHECK(!plan.empty());
    apply_instance(inst, plan);
    // 目录大小写：游戏的 "Textures" 被保留；三层的 base.dds 由最高层 ModB 胜出
    fs::path farm = inst.farm_path;
    CHECK(fs::is_symlink(farm / "Data" / "Textures" / "base.dds"));
    CHECK_EQ(fs::read_symlink(farm / "Data" / "Textures" / "base.dds").string(),
             (inst_p / "mods" / "ModB" / "TEXTURES" / "base.dds").string());
    CHECK(fs::is_symlink(farm / "Data" / "Textures" / "a.dds"));
    CHECK(fs::is_symlink(farm / "SkyrimSE.exe"));
    CHECK(!fs::exists(farm / "Data" / "textures"));
    // 幂等
    CHECK(plan_instance(inst, build_farm_model(inst)).empty());
    // 禁用 ModB → base.dds 回落到 ModA
    CHECK(set_mod_enabled(inst, "ModB", false));
    auto p2 = plan_instance(inst, build_farm_model(inst));
    CHECK(!p2.empty());
    apply_instance(inst, p2);
    CHECK_EQ(fs::read_symlink(farm / "Data" / "Textures" / "base.dds").string(),
             (inst_p / "mods" / "ModA" / "Textures" / "base.dds").string());
    CHECK(!fs::exists(farm / "Data" / "b.esp"));
    CHECK(plan_instance(inst, build_farm_model(inst)).empty());
}

TEST(overwrite_root_is_the_highest_root_layer) {
    Tmp t; fs::path inst_p, game;
    make_fixture(t, inst_p, game);
    // 工具在农场根写下的文件（如降级后的 exe）被 capture 收进 overwrite-root：它要盖过游戏本体
    touch(inst_p / "overwrite-root" / "SkyrimSE.exe", "v16");
    touch(inst_p / "overwrite-root" / "enblocal.ini", "enb");
    auto inst = load_instance(inst_p.string());
    auto model = build_farm_model(inst);
    CHECK_EQ(std::string(model.layer_names.back()), std::string("<overwrite-root>"));
    apply_instance(inst, plan_instance(inst, model));
    fs::path farm = inst.farm_path;
    CHECK_EQ(fs::read_symlink(farm / "SkyrimSE.exe").string(), (inst_p / "overwrite-root" / "SkyrimSE.exe").string());
    CHECK(fs::is_symlink(farm / "enblocal.ini"));
    // 删掉 overwrite-root 里的文件 = 撤销，回到游戏本体
    fs::remove(inst_p / "overwrite-root" / "SkyrimSE.exe");
    apply_instance(inst, plan_instance(inst, build_farm_model(inst)));
    CHECK_EQ(fs::read_symlink(farm / "SkyrimSE.exe").string(), (game / "SkyrimSE.exe").string());
}

TEST(farm_refuses_foreign_dir) {
    Tmp t; fs::path inst_p, game;
    make_fixture(t, inst_p, game);
    auto inst = load_instance(inst_p.string());
    touch(fs::path(inst.farm_path) / "user_file.txt");
    bool threw = false;
    try { plan_instance(inst, build_farm_model(inst)); } catch (const Error& e) { threw = e.code == "farm_not_owned"; }
    CHECK(threw);
}

TEST(build_model_needs_game_dir) {
    Tmp t;
    const std::string root_s = (t.dir / "i").string(), game_s = (t.dir / "missing").string();
    InitOptions o;
    o.root = root_s;
    o.game_dir = game_s;
    init_instance(o);
    auto inst = load_instance(root_s);
    bool threw = false;
    try { build_farm_model(inst); } catch (const Error& e) { threw = e.code == "config_invalid"; }
    CHECK(threw);
}
