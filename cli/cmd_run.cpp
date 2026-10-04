// run：capture 上次残留 → plugins sync → apply 农场 → onAboutToRun → 启动 → 退出后 capture overwrite。
// 会写 profile、前缀 AppData 与农场，并真正启动游戏（--dry-run 只输出将要执行的命令，不碰进程）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/casefold.hpp"
#include "mol/game_host.hpp"
#include "mol/instance.hpp"
#include "mol/overwrite.hpp"
#include "mol/plugins_sync.hpp"
#include "mol/runner.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

Result run_run(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const bool dry = ctx.args.get_bool("--dry-run", false);
    const bool detach = ctx.args.get_bool("--detach", false);
    const bool skse = ctx.args.get_bool("--skse", false);
    mol::string exe = ctx.args.get("--exe", "", ctx.mem);
    if (exe.empty()) exe = skse ? "skse64_loader.exe" : "SkyrimSE.exe";

    const mol::Instance inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const auto host = mol::GameHost::open();
    const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user);

    if (!dry) mol::require_farm_idle(inst);
    std::size_t captured_before = 0;
    bool synced = false;
    if (!dry) {
        captured_before = mol::capture_overwrite(inst);
        mol::sync_plugins(inst, game, ctx.mem);
        synced = true;
    }
    const mol::FarmModel model = mol::build_farm_model(inst, ctx.profile_override(), ctx.mem);
    const mol::Plan plan = mol::plan_instance(inst, model, ctx.mem);
    if (!dry) {
        const bool needs_marker = !path_exists(std::string(inst.farm_path) + "/.mol-farm.json");
        if (!plan.ops.empty() || needs_marker) mol::apply_instance(inst, plan);
    }

    // 先看农场里（或将要出现的农场里）有没有这个可执行文件：大小写不敏感。
    bool exe_found = false;
    for (const auto& op : plan.ops) {
        if (op.kind == mol::OpKind::Link && mol::casefold(op.path) == mol::casefold(exe)) exe_found = true;
    }
    if (!exe_found && !dry) exe_found = path_exists(std::string(inst.farm_path) + "/" + std::string(exe));
    if (!exe_found && dry) exe_found = true;  // dry-run 不应用农场；不在此处拒绝
    if (!exe_found)
        throw mol::Error("mod_not_found", "executable not found in the farm: " + std::string(exe), std::string(exe));

    mol::LaunchSpec spec = mol::build_launch(inst, exe, {}, ctx.mem);
    int game_exit = 0;
    std::size_t captured_after = 0;
    if (!dry) {
        if (!game.about_to_run(exe)) throw mol::Error("game_unavailable", "an onAboutToRun handler refused to run", std::string(exe));
        game_exit = mol::spawn_launch(spec, !detach);
        if (!detach) captured_after = mol::capture_overwrite(inst);
    }

    RunData d{.exe = mol::string(exe, ctx.mem),
              .dry_run = dry,
              .synced_plugins = synced,
              .game_exit_code = game_exit,
              .captured = captured_before + captured_after,
              .argv = std::pmr::vector<std::pmr::string>(ctx.mem),
              .cwd = mol::string(spec.cwd, ctx.mem)};
    for (const auto& a : spec.argv) d.argv.push_back(std::pmr::string(a, ctx.mem));
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
