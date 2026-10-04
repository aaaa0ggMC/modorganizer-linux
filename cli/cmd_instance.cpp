// mo-linux instance init / instance show 子命令。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/instance.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

namespace {

// 注意：instance init 的选项规格表在 cli/args.hpp（kOptInstanceInit），由 main 注册、
// cli/parse.cpp 校验，本文件不再维护副本（规格表唯一来源）。

ConfigData to_config(const mol::InstanceConfig& c, mol::mr* mem) {
    return ConfigData{
        .game = mol::string(c.game, mem),
        .game_dir = mol::string(c.game_dir, mem),
        .prefix = mol::string(c.prefix, mem),
        .prefix_user = mol::string(c.prefix_user, mem),
        .profile = mol::string(c.profile, mem),
        .farm_dir = mol::string(c.farm_dir, mem),
        .runner_kind = mol::string(c.runner_kind, mem),
        .proton_path = mol::string(c.proton_path, mem),
        .steam_root = mol::string(c.steam_root, mem),
    };
}

}  // namespace

Result run_instance_init(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);

    const mol::string game_dir = ctx.args.get("--game-dir", "", ctx.mem);
    const mol::string prefix = ctx.args.get("--prefix", "", ctx.mem);
    const mol::string prefix_user = ctx.args.get("--prefix-user", "steamuser", ctx.mem);
    const mol::string runner = ctx.args.get("--runner", "proton", ctx.mem);
    const mol::string proton_path = ctx.args.get("--proton-path", "", ctx.mem);
    const mol::string steam_root = ctx.args.get("--steam-root", "", ctx.mem);
    if (game_dir.empty()) return make_usage_error("instance init: --game-dir is required", ctx);
    if (prefix.empty()) return make_usage_error("instance init: --prefix is required", ctx);
    if (runner != "proton" && runner != "wine") {
        return make_usage_error("instance init: --runner must be 'proton' or 'wine'", ctx);
    }
    const mol::string profile =
        ctx.globals != nullptr && ctx.globals->has_profile
            ? mol::string(ctx.globals->profile, ctx.mem)
            : mol::string("Default", ctx.mem);

    mol::InitOptions opt;
    opt.root = std::string_view(ctx.instance_dir);
    opt.game = "skyrimse";  // 规格未暴露 --game，用默认
    opt.game_dir = std::string_view(game_dir);
    opt.prefix = std::string_view(prefix);
    opt.prefix_user = std::string_view(prefix_user);
    opt.profile = std::string_view(profile);
    opt.runner_kind = std::string_view(runner);
    opt.proton_path = std::string_view(proton_path);
    opt.steam_root = std::string_view(steam_root);

    const bool changed = mol::init_instance(opt);
    // init 之后再读一次，保证 data.config 与磁盘上的 mo-linux.json 完全一致
    const mol::Instance inst = mol::load_instance(ctx.instance_dir, profile, ctx.mem);

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(InstanceInitData{
        .root = mol::string(inst.root, ctx.mem),
        .changed = changed,
        .config = to_config(inst.cfg, ctx.mem),
    });
    return r;
}

Result run_instance_show(Context& ctx) {
    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(InstanceShowData{
        .root = mol::string(inst.root, ctx.mem),
        .mods_dir = mol::string(inst.mods_dir, ctx.mem),
        .profiles_dir = mol::string(inst.profiles_dir, ctx.mem),
        .downloads_dir = mol::string(inst.downloads_dir, ctx.mem),
        .overwrite_dir = mol::string(inst.overwrite_dir, ctx.mem),
        .farm_path = mol::string(inst.farm_path, ctx.mem),
        .config = to_config(inst.cfg, ctx.mem),
    });
    return r;
}

}  // namespace cli
