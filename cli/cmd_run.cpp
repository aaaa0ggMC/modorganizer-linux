// run：capture 上次残留 → plugins sync → apply 农场 → onAboutToRun → 启动 → 退出后 capture overwrite。
// 会写 profile、前缀 AppData 与农场，并真正启动游戏（--dry-run 只输出将要执行的命令，不碰进程）。
// 默认注入写时复制（libmol-cow.so）：游戏/工具以写方式打开农场里的文件时先换成副本，原文件永远不动；
// 退出后改过的副本收进 overwrite（Data/ 下）或 overwrite-root（根目录，作为最高层参与合并）。
// 游戏层库（Qt）只在启动游戏本体时必需；其它工具（降级补丁、BodySlide……）没有它也能跑，只是不同步 plugins.txt。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <optional>

#include "mol/casefold.hpp"
#include "mol/executables.hpp"
#include "mol/game_host.hpp"
#include "mol/instance.hpp"
#include "mol/overwrite.hpp"
#include "mol/plugins.hpp"
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
    const mol::string title = ctx.args.get("--title", "", ctx.mem);
    if (!title.empty() && !exe.empty()) return make_usage_error("run: --title and --exe are mutually exclusive", ctx);
    if (!title.empty() && skse) return make_usage_error("run: --title and --skse are mutually exclusive", ctx);
    if (exe.empty() && title.empty()) exe = skse ? "skse64_loader.exe" : "SkyrimSE.exe";

    const mol::Instance inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    mol::vector<mol::string> tool_args(ctx.mem);
    if (!title.empty()) {  // 实例里登记的工具：农场里的相对路径，或（农场之外的）绝对路径
        const mol::Executable* hit = nullptr;
        const auto execs = mol::list_executables(inst, ctx.mem);
        for (const auto& e : execs) if (mol::casefold(e.title) == mol::casefold(title)) { hit = &e; break; }
        if (!hit) throw mol::Error("mod_not_found", "no executable titled '" + std::string(title) + "' is registered in this instance (see `executables list`)");
        exe = hit->farm_path.empty() ? hit->binary : hit->farm_path;
        tool_args = mol::split_arguments(hit->arguments, ctx.mem);
    }
    // 游戏本体（或 SKSE 加载器）必须有游戏层；其它工具没有也行
    const std::string exe_name = std::string(mol::casefold(std::string_view(exe).substr(std::string_view(exe).find_last_of("/\\") == std::string_view::npos ? 0 : std::string_view(exe).find_last_of("/\\") + 1)));
    const bool is_game = exe_name == "skyrimse.exe" || exe_name == "skse64_loader.exe";
    std::optional<mol::GameHost> host;
    std::optional<mol::Game> game;
    try {
        host.emplace(mol::GameHost::open());
        game.emplace(host->create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user));
    } catch (const mol::Error& e) {
        if (is_game || e.code != "game_unavailable") throw;
    }

    if (!dry) mol::require_farm_idle(inst);
    std::size_t captured_before = 0;
    bool synced = false;
    if (!dry) {
        captured_before = mol::capture_overwrite(inst);
        {  // 把新装 mod 带来的插件、规范化后的顺序固化进 profile，游戏才会加载它们
            std::vector<mol::string> forced;
            for (const auto& n : forced_plugin_names(ctx, inst)) forced.emplace_back(n, ctx.mem);
            mol::save_plugins(inst, mol::load_plugins(inst, forced, ctx.profile_override(), ctx.mem), ctx.profile_override());
        }
        if (game) {
            mol::sync_plugins(inst, *game, ctx.mem);
            synced = true;
        }
    }
    const mol::FarmModel model = mol::build_farm_model(inst, ctx.profile_override(), ctx.mem);
    const mol::Plan plan = mol::plan_instance(inst, model, ctx.mem);
    if (!dry) {
        const bool needs_marker = !path_exists(std::string(inst.farm_path) + "/.mol-farm.json");
        if (!plan.ops.empty() || needs_marker) mol::apply_instance(inst, plan);
    }

    // 先看农场里（或将要出现的农场里）有没有这个可执行文件：大小写不敏感。
    bool exe_found = false;
    if (!exe.empty() && exe.front() == '/') exe_found = path_exists(std::string(exe));  // 农场之外的工具
    for (const auto& op : plan.ops) {
        if (op.kind == mol::OpKind::Link && mol::casefold(op.path) == mol::casefold(exe)) exe_found = true;
    }
    if (!exe_found && !dry && exe.front() != '/') exe_found = path_exists(std::string(inst.farm_path) + "/" + std::string(exe));
    if (!exe_found && dry && exe.front() != '/') exe_found = true;  // dry-run 不应用农场；不在此处拒绝
    if (!exe_found)
        throw mol::Error("mod_not_found", "executable not found in the farm: " + std::string(exe), std::string(exe));

    if (ctx.args.has("--args")) {
        if (!title.empty()) return make_usage_error("run: --args cannot be combined with --title (the registered executable has its own arguments)", ctx);
        tool_args = mol::split_arguments(ctx.args.get("--args", "", ctx.mem), ctx.mem);
    }
    std::vector<std::string_view> arg_views;
    for (const auto& a : tool_args) arg_views.push_back(a);
    mol::LaunchOptions lo;
    lo.args = arg_views;
    const mol::string cow_lib = ctx.args.get_bool("--no-cow", false) ? mol::string(ctx.mem) : mol::find_cow_library(ctx.mem);
    const std::string cow_log = mol::cow_log_path(inst);
    lo.cow_library = cow_lib;
    lo.cow_log = cow_log;
    mol::LaunchSpec spec = mol::build_launch(inst, exe, lo, ctx.mem);
    int game_exit = 0;
    std::size_t captured_after = 0;
    mol::CowStats cow;
    if (!dry) {
        if (game && !game->about_to_run(exe)) throw mol::Error("game_unavailable", "an onAboutToRun handler refused to run", std::string(exe));
        mol::ensure_wineserver_cow(inst, cow_lib);
        game_exit = mol::spawn_launch(spec, !detach);
        if (!detach) {
            cow = mol::cow_stats(inst);
            captured_after = mol::capture_overwrite(inst);
        }
    }

    RunData d{.exe = mol::string(exe, ctx.mem),
              .title = mol::string(title, ctx.mem),
              .dry_run = dry,
              .detached = detach,
              .synced_plugins = synced,
              .game_exit_code = game_exit,
              .captured = captured_before + captured_after,
              .argv = std::pmr::vector<std::pmr::string>(ctx.mem),
              .cwd = mol::string(spec.cwd, ctx.mem),
              .cow = !cow_lib.empty(),
              .cow_library = mol::string(cow_lib, ctx.mem),
              .cow_copies = cow.copies,
              .cow_reflinked = cow.reflinked};
    for (const auto& a : spec.argv) d.argv.push_back(std::pmr::string(a, ctx.mem));
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

Result run_executables_list(Context& ctx) {
    const mol::Instance inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    ExecutablesData d{.executables = std::pmr::vector<ExecutableRow>(ctx.mem)};
    for (const auto& e : mol::list_executables(inst, ctx.mem))
        d.executables.push_back(ExecutableRow{.title = std::pmr::string(e.title, ctx.mem), .binary = std::pmr::string(e.binary, ctx.mem), .arguments = std::pmr::string(e.arguments, ctx.mem),
                                              .working_dir = std::pmr::string(e.working_dir, ctx.mem), .farm_path = std::pmr::string(e.farm_path, ctx.mem), .hide = e.hide});
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
