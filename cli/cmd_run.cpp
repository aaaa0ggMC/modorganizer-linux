// run：capture 上次残留 → plugins sync → apply 农场 → onAboutToRun → 启动 → 退出后 capture overwrite。
// 会写 profile、前缀 AppData 与农场，并真正启动游戏（--dry-run 只输出将要执行的命令，不碰进程）。
// 默认注入写时复制（libmol-cow.so）：游戏/工具以写方式打开农场里的文件时先换成副本，原文件永远不动；
// 退出后改过的副本收进 overwrite（Data/ 下）或 overwrite-root（根目录，作为最高层参与合并）。
// 游戏层库（Qt）只在启动游戏本体时必需；其它工具（降级补丁、BodySlide……）没有它也能跑，只是不同步 plugins.txt。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>
#include <optional>

#include "mol/casefold.hpp"
#include "mol/executables.hpp"
#include "mol/game_host.hpp"
#include "mol/health.hpp"
#include "mol/instance.hpp"
#include "mol/overwrite.hpp"
#include "mol/plugins.hpp"
#include "mol/plugins_sync.hpp"
#include "mol/runner.hpp"
#include "mol/terminate.hpp"

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
    const bool diagnose = ctx.args.get_bool("--diagnose", false);
    if (diagnose && detach) return make_usage_error("run: --diagnose waits for the game to exit; it cannot be combined with --detach", ctx);
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

    // 启动前检查（docs/PLAN-autofix.md D8）：已知会让游戏闪退/插件大面积失效的前缀问题，先修再启动
    if (is_game && !dry && !ctx.args.get_bool("--force", false)) {
        namespace h = mol::health;
        std::string why, fix;
        bool steam_ok = inst.cfg.runner_kind != "proton" || h::steam_client_running(inst.cfg.steam_root);
        if (!steam_ok && !ctx.args.get_bool("--no-start-steam", false)) {
            if (!(ctx.globals && ctx.globals->quiet)) std::fprintf(stderr, "run: the Steam client is not running; starting it and waiting for it to log in…\n");
            steam_ok = h::start_steam_and_wait(inst.cfg.steam_root);
        }
        if (!steam_ok) {
            why = "the Steam client is not running (or did not log in within 2 minutes): the game would fail SteamAPI_Init and ask Steam to start its own copy of the game (no mods, no copy-on-write)";
            fix = "";
        } else if (const auto lost = mol::plugins_lost_since_snapshot(inst, ctx.profile_override(), ctx.mem); !lost.empty()) {
            // 不拦的话，下面的 save_plugins 会把被改坏的列表当成新的「最后一次写的」
            why = std::to_string(lost.size()) + " plugin(s) were disabled behind mo-linux's back (e.g. " + std::string(lost.front()) +
                  "): the game rewrote plugins.txt";
            fix = "plugins restore";
        } else if (const auto rt = h::vc_runtime(inst); !h::vc_runtime_ok(rt)) {
            why = "the prefix's VC++ runtime is " + (rt.front().version.empty() ? std::string("missing") : rt.front().version) + "; most SKSE plugins need 14.40+ and fail to load";
            fix = "fix vcrun";
        } else {
            std::ifstream in(h::appdata_dir(inst) / "ContentCatalog.txt", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (const auto bad = h::bad_catalog_versions(text); !bad.empty() && h::catalog_versions_harmful(game_info_string(ctx, inst, "version"))) {
                why = "ContentCatalog.txt has entries this game version cannot parse (\"" + bad.front() + "\"); the game would exit a few seconds after start";
                fix = "fix content-catalog";
            }
        }
        if (!why.empty())
            throw mol::Error("prefix_unhealthy", why + (fix.empty() ? std::string(": start Steam and wait until it is logged in, then run again (or `run --force`)")
                                                                     : ": run `mo-linux " + fix + "` first (or `run --force`)"));
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
    std::string log_dir;
    if (diagnose) {
        if (inst.cfg.runner_kind != "proton") return make_usage_error("run: --diagnose needs the proton runner", ctx);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char stamp[32];
        std::tm tm{};
        localtime_r(&now, &tm);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        log_dir = std::string(inst.root) + "/logs/diagnose-" + stamp;
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
        spec.env.emplace_back(mol::string("PROTON_LOG", ctx.mem), mol::string("1", ctx.mem));
        spec.env.emplace_back(mol::string("PROTON_LOG_DIR", ctx.mem), mol::string(log_dir, ctx.mem));
        spec.env.emplace_back(mol::string("WINEDEBUG", ctx.mem), mol::string("+seh,+loaddll", ctx.mem));
    }
    if (detach || diagnose) {  // 后台的游戏输出进日志文件，不占调用方的管道
        if (log_dir.empty()) {
            const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            char stamp[32];
            std::tm tm{};
            localtime_r(&now, &tm);
            std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
            std::error_code ec;
            std::filesystem::create_directories(std::string(inst.root) + "/logs", ec);
            spec.log_file = mol::string(std::string(inst.root) + "/logs/run-" + stamp + ".log", ctx.mem);
        } else {
            spec.log_file = mol::string(log_dir + "/game-output.log", ctx.mem);
        }
    }
    const auto started = std::filesystem::file_time_type::clock::now();
    int game_exit = 0;
    std::size_t captured_after = 0;
    mol::CowStats cow;
    if (!dry) {
        if (game && !game->about_to_run(exe)) throw mol::Error("game_unavailable", "an onAboutToRun handler refused to run", std::string(exe));
        mol::ensure_wineserver_cow(inst, cow_lib);
        if (diagnose) {
            // 等的是 SkyrimSE.exe 本身：skse64_loader.exe 拉起游戏后自己就退出；游戏崩溃后 CrashLogger 会开一个
            // notepad 显示日志，它让 Proton 一直不返回——游戏进程没了就开始诊断，不等那个窗口
            if (!(ctx.globals && ctx.globals->quiet)) std::fprintf(stderr, "run --diagnose: waiting for the game to exit…\n");
            mol::spawn_launch(spec, false);
            const auto t0 = std::chrono::steady_clock::now();
            bool seen = false;
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                bool running = false;
                for (const auto& p : mol::instance_processes(inst))
                    if (mol::casefold(p.command).find("skyrimse.exe") != std::string::npos) running = true;
                seen = seen || running;
                if (seen && !running) break;
                if (!seen && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(90) && !mol::farm_in_use(inst)) break;  // 根本没起来
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));  // 让崩溃日志 / Proton 日志写完
        } else {
            game_exit = mol::spawn_launch(spec, !detach);
        }
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
    if (diagnose && !dry) {
        namespace h = mol::health;
        d.log_file = std::pmr::string(log_dir + "/steam-489830.log", ctx.mem);
        auto say = [&](const std::string& s) { d.diagnosis.push_back(std::pmr::string(s, ctx.mem)); };
        std::ifstream in(std::string(d.log_file), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.empty()) say("no Proton log was written (" + std::string(d.log_file) + ")");
        const auto cr = h::latest_crash(inst);
        const bool crashed = !cr.file.empty() && cr.mtime >= started;
        if (crashed) {
            say("the game crashed; CrashLogger wrote " + cr.file);
            for (const auto& l : h::crash_summary_lines(cr)) say("  " + l);
        }
        const auto pd = h::parse_proton_log(text);
        if (pd.last) {
            say("last notable exception: " + h::describe_exception_code(pd.last->code) + " (0x" + pd.last->code + ")" +
                (pd.last->module.empty() ? std::string() : " raised in " + pd.last->module));
            std::string all;
            for (const auto& [c, n] : pd.codes) all += (all.empty() ? "" : ", ") + c + " x" + std::to_string(n);
            say("notable exception codes: " + all + (crashed ? " (a modded game raises many of these normally; the crash log above is the reliable part)" : ""));
            if (pd.last->code == "e06d7363" && mol::casefold(pd.last->module).starts_with("msvcp140"))
                say("hint: a C++ exception from MSVCP140 right after start is typical of an unreadable ContentCatalog.txt (`fix content-catalog`) or an old VC++ runtime (`fix vcrun`)");
        } else if (!text.empty()) {
            say("no notable exception in the Proton log (the game exited on its own)");
        }
        if (!pd.gpu_used.empty()) {
            say("GPU: " + pd.gpu_used + (pd.gpus.size() > 1 ? " (" + std::to_string(pd.gpus.size()) + " adapters found)" : ""));
            if (const std::string dg = h::discrete_gpu_unused(pd); !dg.empty())
                say("hint: the game ran on the integrated GPU although '" + dg + "' is present; force it with DXVK_FILTER_DEVICE_NAME=\"" + dg +
                    "\" (NVIDIA hybrid laptops also need __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia) in the environment of `mo-linux run`");
        }
        const auto sl = h::read_skse_log(inst);
        if (sl.found && sl.mtime >= started)
            say(std::to_string(sl.loaded) + " SKSE plugin(s) loaded, " + std::to_string(sl.failed.size()) + " failed" +
                (sl.failed.empty() ? std::string() : " (" + sl.failed.front() + (sl.failed.size() > 1 ? ", …" : "") + ")"));
        else
            say("SKSE did not write a log this run (the game ended before SKSE loaded, or it was not started through SKSE)");
    }
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    if (cow_lib.empty() && !ctx.args.get_bool("--no-cow", false))
        r.add_warning("cow_unavailable", "libmol-cow.so not found next to mo-linux (or $MOL_COW_LIB): writes to existing files went through to the originals", "");
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
