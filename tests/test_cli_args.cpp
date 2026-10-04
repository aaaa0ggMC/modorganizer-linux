// CLI 参数处理的单测：直接驱动 alib6 Command（真路由 + 真解析），
// 验证 CommandInput → GlobalOptions/ParsedArgs 适配器（cli/parse.cpp）、
// 位置参数个数校验与 MOL_INSTANCE 回退。
//
// 混用约束（见 cli/cmd_common.hpp）：所有 #include 在 import 之前。
#include <string>
#include <vector>

#include "minitest.hpp"
#include "../cli/args.cpp"
#include "../cli/cmd_common.cpp"
#include "../cli/parse.cpp"

import alib6;
import std;

namespace {

using namespace cli;
using alib6::Command;
using CommandInput = Command::CommandInput;
using CommandOutput = Command::CommandOutput;

std::pmr::monotonic_buffer_resource& arena() {
    static std::pmr::monotonic_buffer_resource pool;
    return pool;
}

mol::mr* mem() { return &arena(); }

// 一次「跑 CLI」的捕获结果
struct Capture {
    GlobalOptions globals;
    ParsedArgs args;
    std::string global_err;
    std::string command;
    bool help = false;
    bool ran = false;

    Capture() : globals(mol::allocator_type(mem())), args(mol::allocator_type(mem())) {}
};

// 用真正的 alib6 Command 跑一遍 argv。
//   route_path  : alib6 路由（如 "mods/move"）
//   allowed     : 该命令的专有选项（parse 校验用）
//   registered  : 额外注册的选项（模拟其它命令的选项，用于「注册了但本命令不允许」用例）
Capture run(std::string_view route_path, std::span<const OptionSpec> allowed,
            std::span<const OptionSpec> registered, std::initializer_list<const char*> items) {
    Capture cap;
    std::pmr::monotonic_buffer_resource cmd_arena;
    Command cmd(&cmd_arena);
    auto register_opts = [&cmd](std::span<const OptionSpec> specs) {
        for (const auto& o : specs) {
            cmd.register_option({.name = o.name,
                                 .short_name = o.short_name,
                                 .long_name = o.long_name,
                                 .description = o.description});
        }
    };
    register_opts(cli::kOptGlobal);
    for (const auto& t : cli::kTogGlobal) {
        cmd.register_toggle({.name = t.name,
                             .short_name = t.short_name,
                             .long_name = t.long_name,
                             .description = t.description});
    }
    register_opts(allowed);
    register_opts(registered);
    cmd.add_route(route_path,
                  [&](const CommandInput& in) -> CommandOutput {
                      cap.global_err = std::string(build_global_options(in, cap.globals, mem()));
                      build_parsed_args(in, allowed, cap.args, mem());
                      std::string name;
                      for (const auto r : in.routes) {
                          if (!name.empty()) name += " ";
                          name += std::string(r);
                      }
                      cap.command = name;
                      cap.help = in.has("help");
                      cap.ran = true;
                      return CommandOutput::with_code(0);
                  });
    std::vector<const char*> av;
    av.push_back("mo-linux");
    for (const char* i : items) av.push_back(i);
    (void)cmd.from_args(static_cast<int>(av.size()), av.data());
    return cap;
}

Capture run(std::string_view route_path, std::span<const OptionSpec> allowed,
            std::initializer_list<const char*> items) {
    return run(route_path, allowed, {}, items);
}

}  // namespace

// ---------------------------------------------------------------------------
// 全局选项：命令前 / 命令后 / 位置参数后 都应识别
// ---------------------------------------------------------------------------
TEST(global_options_before_the_command) {
    const Capture c = run("mods/list", {}, {"-j", "-i", "/x", "-q", "mods", "list"});
    CHECK(c.ran);
    CHECK(c.global_err.empty());
    CHECK(c.globals.json);
    CHECK(c.globals.quiet);
    CHECK(c.globals.has_instance);
    CHECK_EQ(std::string(c.globals.instance), std::string("/x"));
}

TEST(global_options_after_the_command) {
    const Capture c = run("mods/list", {}, {"mods", "list", "-j", "--profile", "P"});
    CHECK(c.global_err.empty());
    CHECK(c.globals.json);
    CHECK(c.globals.has_profile);
    CHECK_EQ(std::string(c.globals.profile), std::string("P"));
    CHECK(c.args.ok());
    CHECK(c.args.positionals.empty());
}

TEST(global_options_after_a_positional) {
    const Capture c = run("mods/enable", {}, {"mods", "enable", "ModA", "-j"});
    CHECK(c.global_err.empty());
    CHECK(c.globals.json);
    CHECK(c.args.ok());
    CHECK_EQ(c.args.positionals.size(), std::size_t{1});
    CHECK_EQ(std::string(c.args.positionals.front()), std::string("ModA"));
}

TEST(long_and_inline_forms) {
    const Capture c =
        run("mods/list", {}, {"--instance=/x", "--json", "--profile=P", "mods", "list"});
    CHECK(c.global_err.empty());
    CHECK(c.globals.json);
    CHECK_EQ(std::string(c.globals.instance), std::string("/x"));
    CHECK_EQ(std::string(c.globals.profile), std::string("P"));

    const Capture s = run("mods/list", {}, {"-i=/y", "mods", "list"});
    CHECK(s.global_err.empty());
    CHECK_EQ(std::string(s.globals.instance), std::string("/y"));
}

TEST(events_option_is_captured) {
    const Capture c = run("apply", {}, {"apply", "--events", "fd:3"});
    CHECK(c.global_err.empty());
    CHECK(c.globals.has_events);
    CHECK_EQ(std::string(c.globals.events), std::string("fd:3"));
    CHECK(!c.globals.json);
}

// ---------------------------------------------------------------------------
// 子命令选项与位置参数
// ---------------------------------------------------------------------------
TEST(subcommand_option_after_positional) {
    const Capture c = run("mods/move", cli::kOptModsMove, {"mods", "move", "ModA", "--to", "3"});
    CHECK(c.global_err.empty());
    CHECK(c.args.ok());
    CHECK_EQ(c.args.positionals.size(), std::size_t{1});
    CHECK(c.args.has("--to"));
    CHECK_EQ(std::string(c.args.get("--to", "", mem())), std::string("3"));
}

TEST(subcommand_option_inline_and_before_positional) {
    const Capture c = run("mods/move", cli::kOptModsMove, {"mods", "move", "--to=3", "ModA"});
    CHECK(c.args.ok());
    CHECK_EQ(c.args.positionals.size(), std::size_t{1});
    CHECK_EQ(std::string(c.args.get("--to", "", mem())), std::string("3"));
}

TEST(get_bool_switch_present_is_true) {
    ParsedArgs a{mol::allocator_type(mem())};
    a.options.emplace_back(mol::string("--dry-run", mem()), mol::string(mem()));  // 开关：存在即值为空
    a.options.emplace_back(mol::string("--off", mem()), mol::string("false", mem()));
    CHECK(a.get_bool("--dry-run", false));
    CHECK(!a.get_bool("--off", true));
    CHECK(!a.get_bool("--absent", false));
}

TEST(conflicts_mod_option) {
    const Capture c = run("conflicts", cli::kOptConflicts, {"conflicts", "--mod", "ModA"});
    CHECK(c.args.ok());
    CHECK_EQ(std::string(c.args.get("--mod", "none", mem())), std::string("ModA"));
}

TEST(dashdash_makes_everything_a_positional) {
    // "--" 之后的 "-j" 不再是开关
    const Capture a = run("mods/list", {}, {"mods", "list", "--", "-j"});
    CHECK(a.global_err.empty());
    CHECK(!a.globals.json);
    CHECK(a.args.ok());
    CHECK_EQ(a.args.positionals.size(), std::size_t{1});
    CHECK_EQ(std::string(a.args.positionals.front()), std::string("-j"));

    // 位置参数本身看起来像选项
    const Capture b = run("mods/move", cli::kOptModsMove, {"mods", "move", "--", "-weird"});
    CHECK(b.args.ok());
    CHECK_EQ(b.args.positionals.size(), std::size_t{1});
    CHECK_EQ(std::string(b.args.positionals.front()), std::string("-weird"));
}

TEST(help_toggle_is_visible_everywhere) {
    const Capture a = run("mods/list", {}, {"-h", "mods", "list"});
    CHECK(a.help);
    const Capture b = run("mods/list", {}, {"mods", "list", "--help"});
    CHECK(b.help);
}

// ---------------------------------------------------------------------------
// 用法错误（invalid_argument）：不可误执行变更
// ---------------------------------------------------------------------------
TEST(unregistered_option_is_rejected) {
    const Capture c = run("mods/list", {}, {"mods", "list", "--bogus"});
    CHECK(!c.args.ok());
    CHECK(c.args.error.find("--bogus") != std::string::npos);
}

TEST(option_of_another_command_is_rejected) {
    // --to 已注册（mods move 的选项），但不属于 mods list
    const Capture c = run("mods/list", {}, cli::kOptModsMove, {"mods", "list", "--to", "3"});
    CHECK(!c.args.ok());
    CHECK(c.args.error.find("--to") != std::string::npos);
    CHECK(c.args.error.find("mods list") != std::string::npos);
}

TEST(missing_option_value_is_rejected) {
    const Capture a = run("mods/move", cli::kOptModsMove, {"mods", "move", "ModA", "--to"});
    CHECK(!a.args.ok());
    CHECK(a.args.error.find("--to") != std::string::npos);

    // 值的位置上是另一个选项 → 同样算缺值
    const Capture b = run("mods/list", {}, {"-i", "-j", "mods", "list"});
    CHECK(!b.global_err.empty());
    CHECK(b.global_err.find("--instance") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 位置参数个数校验（check_positionals）
// ---------------------------------------------------------------------------
namespace {
Context make_ctx(std::initializer_list<const char*> positionals, std::string_view command) {
    Context ctx{mol::allocator_type(mem())};
    ctx.mem = mem();
    ctx.command = mol::string(command, mem());
    for (const char* p : positionals) {
        ctx.args.positionals.push_back(mol::string(p, mem()));
    }
    return ctx;
}
}  // namespace

TEST(positionals_exact_count_passes) {
    Context ctx = make_ctx({"ModA"}, "mods enable");
    CHECK(!check_positionals(ctx, 1, "NAME").has_value());
    Context none = make_ctx({}, "conflicts");
    CHECK(!check_positionals(none, 0, "").has_value());
}

TEST(positionals_missing_or_extra_are_usage_errors) {
    Context none = make_ctx({}, "mods enable");
    if (auto bad = check_positionals(none, 1, "NAME")) {
        CHECK_EQ(bad->exit_code, 2);
        CHECK(std::string(bad->errors.front().message).find("NAME is required") !=
              std::string::npos);
    } else {
        CHECK(false);
    }

    Context two = make_ctx({"ModA", "ModB"}, "mods enable");
    if (auto bad = check_positionals(two, 1, "NAME")) {
        CHECK_EQ(bad->exit_code, 2);
        CHECK(std::string(bad->errors.front().message).find("unexpected argument") !=
              std::string::npos);
    } else {
        CHECK(false);
    }

    Context extra = make_ctx({"oops"}, "conflicts");
    if (auto bad = check_positionals(extra, 0, "")) {
        CHECK_EQ(bad->exit_code, 2);
        CHECK(std::string(bad->errors.front().code) == "invalid_argument");
    } else {
        CHECK(false);
    }
}

// ---------------------------------------------------------------------------
// 实例目录：-i > $MOL_INSTANCE > cwd
// ---------------------------------------------------------------------------
TEST(instance_dir_prefers_flag_then_env_then_cwd) {
    GlobalOptions g{mol::allocator_type(mem())};
    g.has_instance = true;
    g.instance = mol::string("rel/inst", mem());
    ::unsetenv("MOL_INSTANCE");
    const std::string from_flag = std::string(resolve_instance_dir(g, mem()));
    CHECK(from_flag.find("rel/inst") != std::string::npos);
    CHECK(from_flag.starts_with("/"));  // 已绝对化

    GlobalOptions g2{mol::allocator_type(mem())};
    ::setenv("MOL_INSTANCE", "/tmp/mol-inst", 1);
    CHECK_EQ(std::string(resolve_instance_dir(g2, mem())), std::string("/tmp/mol-inst"));
    ::unsetenv("MOL_INSTANCE");

    GlobalOptions g3{mol::allocator_type(mem())};
    const std::string from_cwd = std::string(resolve_instance_dir(g3, mem()));
    CHECK(from_cwd.starts_with("/"));
}

TEST(path_exists_detects_tmp_paths) {
    CHECK(path_exists("/tmp"));
    CHECK(!path_exists("/tmp/opencode/definitely-not-here-xyz"));
}

TEST(exit_code_mapping) {
    CHECK_EQ(exit_code_from_issues(mol::vector<Err>{}), 0);
    mol::vector<Err> one{mol::allocator_type(mem())};
    one.push_back(Err{mol::string("invalid_argument", mem()), mol::string("bad", mem()),
                      mol::string("", mem())});
    CHECK_EQ(exit_code_from_issues(one), 2);
    one.clear();
    one.push_back(Err{mol::string("farm_not_owned", mem()), mol::string("boom", mem()),
                      mol::string("", mem())});
    CHECK_EQ(exit_code_from_issues(one), 1);
}

TEST(dashdash_does_not_accept_an_earlier_unknown_option) {
    const auto c = run("mods/enable", {}, {"mods", "enable", "--bogus", "--"});
    CHECK(!c.args.ok());
}

TEST(global_errors_preserve_json_and_quiet) {
    const auto c = run("version", {}, {"-j", "-q", "version", "--instance"});
    CHECK(!c.global_err.empty());
    CHECK(c.globals.json);
    CHECK(c.globals.quiet);
}
