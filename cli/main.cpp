// mo-linux CLI 入口：alib6 Command 解析与路由 → 子命令 → 输出 → 退出码。
//
// ⚠ 混用约束（GCC 16 实测踩坑，后人务必保留）⚠⚠
//   同一 TU 内 `import alib6;` / `import std;` 与「文本 include 标准库头」不能共存
//   （libstdc++ 的模块视图与文本视图会互相重定义）。解决办法：**所有 #include 都排在
//   所有 import 之前**，且一旦某个头文件触发了 import，之后就不能再引入新的标准库文本
//   include。因此：先 include 纯文本头，最后才 include 含 `import alib6;` 的头
//   （cli/output.hpp → cli/cmd_common.hpp，以及 cli/parse.hpp、cli/commands.hpp），
//   顺序不能随意调换——parse.hpp 放在 output.hpp 之后，是为了让它内部的 <span>/
//   <string_view>/args.hpp 都已成为无操作的 include guard 命中。
#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "events.hpp"
#include "mol/error.hpp"
#include "mol/instance.hpp"
#include "output.hpp"
#include "parse.hpp"
#include "commands.hpp"
import alib6;
import std;

namespace {

using namespace alib6;
using CommandOutput = Command::CommandOutput;
using cli::Context;
using cli::Result;

struct RouteEntry {
    std::string_view path;                    // alib6 路由（'/' 分层，见 Router::add_route）
    std::string_view name;                    // 规范命令名（envelope.command / 文本渲染）
    cli::CommandFn fn;
    std::span<const cli::OptionSpec> specs;   // 该命令的专有选项（空表 = 只用全局选项）
    std::size_t positionals = 0;              // 期望的位置参数个数
    std::string_view positional_name;         // 期望 1 个时的参数名（报错用）
};

constexpr RouteEntry kRoutes[] = {
    {"version", "version", &cli::run_version, {}, 0, ""},
    {"game/info", "game info", &cli::run_game_info, {}, 0, ""},
    {"plugins/sync", "plugins sync", &cli::run_plugins_sync, {}, 0, ""},
    {"overwrite/capture", "overwrite capture", &cli::run_overwrite_capture, {}, 0, ""},
    {"overwrite/promote", "overwrite promote", &cli::run_overwrite_promote, cli::kOptPromote, 0, ""},
    {"doctor", "doctor", &cli::run_doctor_cmd, {}, 0, ""},
    {"nexus/login", "nexus login", &cli::run_nexus_login, cli::kOptNexusLogin, 0, ""},
    {"nexus/logout", "nexus logout", &cli::run_nexus_logout, {}, 0, ""},
    {"nexus/whoami", "nexus whoami", &cli::run_nexus_whoami, {}, 0, ""},
    {"nexus/files", "nexus files", &cli::run_nexus_files, cli::kOptNexusFiles, 0, ""},
    {"nexus/download", "nexus download", &cli::run_nexus_download, cli::kOptNexusDownload, 0, ""},
    {"run", "run", &cli::run_run, cli::kOptRun, 0, ""},
    {"instance/init", "instance init", &cli::run_instance_init, cli::kOptInstanceInit, 0, ""},
    {"instance/show", "instance show", &cli::run_instance_show, {}, 0, ""},
    {"mods/list", "mods list", &cli::run_mods_list, {}, 0, ""},
    {"mods/enable", "mods enable", &cli::run_mods_enable, {}, 1, "NAME"},
    {"mods/disable", "mods disable", &cli::run_mods_disable, {}, 1, "NAME"},
    {"mods/move", "mods move", &cli::run_mods_move, cli::kOptModsMove, 1, "NAME"},
    {"conflicts", "conflicts", &cli::run_conflicts, cli::kOptConflicts, 0, ""},
    {"plan", "plan", &cli::run_plan, {}, 0, ""},
    {"status", "status", &cli::run_status, {}, 0, ""},
    {"apply", "apply", &cli::run_apply, {}, 0, ""},
    {"unlink", "unlink", &cli::run_unlink, {}, 0, ""},
};

// 一次调用的可变状态（handler 通过 lambda 把结果写回这里）
struct Invocation {
    cli::Context ctx;
    cli::Result result;
    bool handled = false;      // 命中了叶子路由（result 有效）
    bool help = false;         // 已打印 help 树
    bool fallback_set = false;  // default handler 给出了兜底（未知命令等）
    cli::Result fallback;

    explicit Invocation(mol::allocator_type a) : ctx(a), result(a), fallback(a) {}
};

// ---------------------------------------------------------------------------
// 输出：stdout 只有结果；warnings/errors 走 stderr（alib6 Logger → Console(stderr)）
// ---------------------------------------------------------------------------
int emit(Result r, const cli::GlobalOptions& g, mol::mr* mem, LogFactory* lg) {
    if (g.json) {
        std::println("{}", std::string_view(cli::serialize_envelope(r, mem)));
    } else {
        for (const auto& line : cli::render_text(r, mem)) {
            std::println("{}", std::string_view(line));
        }
    }
    if (lg != nullptr) {
        for (const auto& w : r.warnings) {
            (*lg)(Severity::Warn) << std::string_view(w.code) << ": " << std::string_view(w.message)
                                  << endlog;
        }
        for (const auto& e : r.errors) {
            (*lg)(Severity::Error) << std::string_view(e.code) << ": " << std::string_view(e.message)
                                   << endlog;
        }
        lg->logger.flush();
    }
    return r.exit_code;
}

std::string join_tokens(std::span<const std::string_view> toks) {
    std::string out;
    for (const auto t : toks) {
        if (!out.empty()) out += " ";
        out += std::string(t);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    // 整条命令共享一块栈上 arena，命令结束整体释放
    std::pmr::monotonic_buffer_resource arena;

    // ---- 1. argv → pmr vector -------------------------------------------------
    mol::vector<mol::string> args(&arena);
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.push_back(mol::string(std::string_view(argv[i]), &arena));
    }

    // ---- 2. 日志（stderr；默认 warn+，-q 在解析到全局选项后立即收紧） -------------
    // consumer_count = 0：同步直写，不需要后台线程（CLI 单发单收）。
    // ConsoleConfig::output_target = stderr ⇒ fwrite 真实落在 fd 2（已核对 log.cpp）。
    Logger logger(LoggerConfig{.consumer_count = 0}, &arena);
    lot::ConsoleConfig console_cfg;
    console_cfg.output_target = stderr;
    logger.append_mod<lot::Console>("console", static_cast<std::uint16_t>(0), console_cfg);
    LogFactoryConfig log_cfg;
    log_cfg.header = "mo-linux";
    log_cfg.def_level = static_cast<int>(Severity::Info);
    static constexpr auto kSilent = [](int) { return false; };
    static constexpr auto kWarnAndAbove = [](int level) { return level >= 3; };
    log_cfg.level_should_keep = kWarnAndAbove;
    LogFactory lg(logger, log_cfg);

    // ---- 3. alib6 Command：注册选项/开关（规格表在 cli/args.hpp） -----------------
    cli::GlobalOptions globals{mol::allocator_type(&arena)};
    Command cmd(&arena);
    for (const auto& o : cli::kOptGlobal) {
        cmd.register_option({.name = o.name,
                             .short_name = o.short_name,
                             .long_name = o.long_name,
                             .description = o.description});
    }
    for (const auto& t : cli::kTogGlobal) {
        cmd.register_toggle({.name = t.name,
                             .short_name = t.short_name,
                             .long_name = t.long_name,
                             .description = t.description});
    }
    // 子命令专有选项也必须注册：alib6 只从 remains 里提取「注册过」的选项，
    // 不注册就会被当成未知 token/位置参数。同名去重（多个命令可能共享）。
    auto register_unique = [&cmd](std::span<const cli::OptionSpec> specs) {
        for (const auto& s : specs) {
            bool have = false;
            for (const auto& r : cmd.registered_options) {
                if (r.name == s.name) {
                    have = true;
                    break;
                }
            }
            if (!have && !s.takes_value) {
                cmd.register_toggle({.name = s.name,
                                     .short_name = s.short_name,
                                     .long_name = s.long_name,
                                     .description = s.description});
            } else if (!have) {
                cmd.register_option({.name = s.name,
                                     .short_name = s.short_name,
                                     .long_name = s.long_name,
                                     .description = s.description});
            }
        }
    };
    for (const auto& e : kRoutes) {
        register_unique(e.specs);
    }

    Invocation inv{mol::allocator_type(&arena)};
    cli::EventSink sink;
    inv.ctx.globals = &globals;
    inv.ctx.mem = &arena;

    // 兜底：没有命中任何叶子路由时（未知命令 / 只有 group / 根层选项错误）。
    // group 节点的 dispatcher 是 0，也会走到这里，但 is_main=false —— 等叶子 handler。
    cmd.register_default_handler([&](const Command::CommandInput& in) -> Command::CommandOutput {
        if (!in.is_main) return CommandOutput::no_output();
        // `mo-linux --help`（选项在命令前）也会走到这里：没有叶子 handler，help 得在此打印
        if (in.has("help")) {
            std::println("{}", std::string_view(cmd.help().str(&arena)));
            inv.help = true;
            return CommandOutput::no_output();
        }
        mol::string err = cli::build_global_options(in, globals, &arena);
        // -q 在这条路径也要生效（没有叶子 handler 来收紧过滤器）
        lg.cfg.level_should_keep = globals.quiet ? kSilent : kWarnAndAbove;
        if (err.empty()) {
            std::string line = join_tokens(in.routes);
            const std::string rest = join_tokens(in.args());
            if (!rest.empty()) {
                if (!line.empty()) line += " ";
                line += rest;
            }
            if (line.empty()) line = "(no command)";
            err = mol::string("unknown command: " + line, &arena);
        }
        inv.fallback_set = true;
        cli::Context ectx{mol::allocator_type(&arena)};
        ectx.globals = &globals;
        ectx.mem = &arena;
        ectx.command = mol::string(in.routes.empty()
                                       ? (in.args().empty() ? std::string_view("")
                                                           : in.args().front())
                                       : std::string_view(in.routes.front()),
                                   &arena);
        inv.fallback = cli::make_usage_error(err, ectx);
        return CommandOutput::no_output();
    });

    // ---- 4. 路由 handler：CommandInput → Context → 子命令 → Result ---------------
    auto make_route = [&](const RouteEntry& e) {
        return [&inv, &sink, &globals, &lg, &cmd, &arena, e](const Command::CommandInput& in)
                   -> Command::CommandOutput {
            inv.ctx.command = mol::string(e.name, &arena);
            inv.handled = true;

            // --help：打印 alib6 生成的帮助树，退出 0（不需要跑命令）
            if (in.has("help")) {
                std::println("{}", std::string_view(cmd.help().str(&arena)));
                inv.help = true;
                return CommandOutput::with_code(0);
            }

            // 1) 全局选项（命令前/后、位置参数前后都能识别；缺值 → invalid_argument）
            mol::string err = cli::build_global_options(in, globals, &arena);
            if (!err.empty()) {
                inv.result = cli::make_usage_error(err, inv.ctx);
                return CommandOutput::with_code(inv.result.exit_code);
            }
            // -q 只有在解析后才可能知道：立即收紧日志过滤器
            lg.cfg.level_should_keep = globals.quiet ? kSilent : kWarnAndAbove;

            // 2) 子命令选项 + 位置参数；用法错误在跑命令前拦住
            cli::build_parsed_args(in, e.specs, inv.ctx.args, &arena);
            if (!inv.ctx.args.ok()) {
                inv.result = cli::make_usage_error(inv.ctx.args.error, inv.ctx);
                return CommandOutput::with_code(inv.result.exit_code);
            }
            if (auto bad = cli::check_positionals(inv.ctx, e.positionals, e.positional_name)) {
                inv.result = std::move(*bad);
                return CommandOutput::with_code(inv.result.exit_code);
            }

            // 3) --events 出口（目标格式非法 → 用法错误；fifo 无读端/unix 连不上 → 静默放弃）
            if (globals.has_events) {
                const mol::string eerr = sink.open(globals.events, &arena);
                if (!eerr.empty()) {
                    inv.result = cli::make_usage_error(eerr, inv.ctx);
                    return CommandOutput::with_code(inv.result.exit_code);
                }
                if (!sink.active() && !sink.disabled_reason().empty()) {
                    lg(Severity::Warn) << "events sink disabled: " << sink.disabled_reason()
                                       << endlog;
                    lg.logger.flush();
                }
            }
            inv.ctx.sink = globals.has_events ? &sink : nullptr;
            inv.ctx.instance_dir = cli::resolve_instance_dir(globals, &arena);

            // 4) 跑子命令；mol::Error 保留其稳定 code，其它异常归一为 io_error
            try {
                inv.result = e.fn(inv.ctx);
            } catch (const mol::Error& ex) {
                inv.result = cli::result_from_mol_error(ex, inv.ctx);
            } catch (const std::exception& ex) {
                inv.result = cli::result_from_std_exception(ex, inv.ctx);
            }
            inv.result.command = inv.ctx.command;
            return CommandOutput::with_code(inv.result.exit_code);
        };
    };
    for (const auto& e : kRoutes) {
        cmd.add_route(e.path, make_route(e));
    }

    // ---- 5. 分发（完整 argv 交给 alib6：不再预先摘选项） -------------------------
    std::vector<const char*> av;
    av.push_back("mo-linux");
    for (const auto& t : args) av.push_back(t.c_str());
    const auto outs = cmd.from_args(static_cast<int>(av.size()), av.data());
    (void)outs;  // 结果通过 inv 传递（handler 同步执行）

    if (inv.help) return 0;

    Result result{mol::allocator_type(&arena)};
    if (inv.handled) {
        result = std::move(inv.result);
    } else if (inv.fallback_set) {
        result = std::move(inv.fallback);
    } else {
        // 理论不可达（default handler 总会兜底）；保留以保持契约完整
        cli::Context ectx{mol::allocator_type(&arena)};
        ectx.globals = &globals;
        ectx.mem = &arena;
        std::string line;
        for (const auto& t : args) {
            if (!line.empty()) line += " ";
            line += std::string(t);
        }
        if (line.empty()) line = "(no command)";
        result = cli::result_from_unknown_command(ectx, line);
    }

    // 包括全局选项错误的早返回结果，也要尊重已解析的 -q。
    lg.cfg.level_should_keep = globals.quiet ? kSilent : kWarnAndAbove;
    return emit(std::move(result), globals, &arena, &lg);
}
