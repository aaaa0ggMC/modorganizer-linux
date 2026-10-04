// mo-linux mods list/enable/disable/move 子命令。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <cerrno>
#include <cstdlib>

#include "mol/casefold.hpp"
#include "mol/instance.hpp"
#include "mol/mod_install.hpp"
#include "mol/fomod.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

namespace {

// 非负整数解析（失败 → false）
bool parse_index(std::string_view s, long long& out) {
    const std::string tmp(s);
    if (tmp.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(tmp.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || v < 0) return false;
    out = static_cast<long long>(v);
    return true;
}

// mod 名大小写不敏感比较（MO2 语义）
bool same_name(std::string_view a, std::string_view b, mol::mr* mem) {
    return mol::casefold(a, mem) == mol::casefold(b, mem);
}

}  // namespace

Result run_mods_list(Context& ctx) {
    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::vector<mol::ModInfo> mods = mol::list_mods(inst, ctx.profile_override(), ctx.mem);

    ModsListData d{
        .profile = mol::string(inst.cfg.profile, ctx.mem),
        .mods = mol::vector<ModRow>(ctx.mem),
    };
    d.mods.reserve(mods.size());
    for (const auto& m : mods) {
        d.mods.push_back(ModRow{
            .name = mol::string(m.name, ctx.mem),
            .enabled = m.enabled,
            .separator = m.separator,
            .exists = m.exists,
            .root = m.root,
            .priority = m.priority,
            .path = mol::string(m.path, ctx.mem),
        });
    }

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(d);
    return r;
}

Result run_mods_enable(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    // 位置参数个数由 main 的路由表校验（check_positionals）；此处只做防御性取值
    const mol::string name = ctx.args.positionals.front();
    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const bool changed = mol::set_mod_enabled(inst, name, true, ctx.profile_override());

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(ModToggleData{
        .name = mol::string(name, ctx.mem),
        .enabled = true,
        .changed = changed,
    });
    return r;
}

Result run_mods_disable(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    // 位置参数个数由 main 的路由表校验（check_positionals）；此处只做防御性取值
    const mol::string name = ctx.args.positionals.front();
    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const bool changed = mol::set_mod_enabled(inst, name, false, ctx.profile_override());

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(ModToggleData{
        .name = mol::string(name, ctx.mem),
        .enabled = false,
        .changed = changed,
    });
    return r;
}

Result run_mods_install(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const mol::string archive = ctx.args.positionals.front();
    const mol::string name = ctx.args.get("--name", "", ctx.mem);
    const mol::string fomod_file = ctx.args.get("--fomod", "", ctx.mem);
    const bool defaults = ctx.args.get_bool("--fomod-defaults", false);
    const bool raw = ctx.args.get_bool("--no-fomod", false);
    if ((!fomod_file.empty() ? 1 : 0) + (defaults ? 1 : 0) + (raw ? 1 : 0) > 1)
        return make_usage_error("mods install: --fomod, --fomod-defaults and --no-fomod are mutually exclusive", ctx);
    const mol::Instance inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);

    mol::InstallOptions opt;
    opt.name = name;
    opt.force_root = ctx.args.get_bool("--root", false);
    opt.profile = ctx.profile_override();
    if (raw) opt.fomod = mol::FomodMode::Raw;
    else if (defaults) opt.fomod = mol::FomodMode::Defaults;
    else if (!fomod_file.empty()) {
        std::ifstream in{std::string(fomod_file), std::ios::binary};
        if (!in) throw mol::Error("io_error", "cannot read the FOMOD choices file", std::string(fomod_file));
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        opt.choices = mol::fomod::parse_choices_json(text);
        opt.fomod = mol::FomodMode::Choices;
    }
    // FOMOD 条件里的版本依赖：游戏版本与脚本扩展版本来自游戏层（取不到就用保守默认）
    if (opt.fomod == mol::FomodMode::Defaults || opt.fomod == mol::FomodMode::Choices) {
        if (auto v = game_info_string(ctx, inst, "version"); !v.empty()) opt.fomod_env.game_version = v;
        if (auto v = game_info_string(ctx, inst, "scriptExtender", "version"); !v.empty()) opt.fomod_env.script_extender_version = v;
    }

    const auto res = mol::install_archive(inst, archive, opt, ctx.mem);
    ModInstallData d{.name = mol::string(res.name, ctx.mem), .path = mol::string(res.path, ctx.mem),
                     .root = res.root, .files = res.files, .fomod = res.fomod,
                     .missing = std::pmr::vector<std::pmr::string>(ctx.mem)};
    for (const auto& m : res.missing) d.missing.push_back(std::pmr::string(m, ctx.mem));
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    for (const auto& m : res.missing) r.add_warning("fomod_missing_source", "FOMOD references a file that is not in the archive", m);
    r.set_data(std::move(d));
    return r;
}

Result run_mods_move(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    // 位置参数个数由 main 的路由表校验（check_positionals）；此处只做防御性取值
    const mol::string name = ctx.args.positionals.front();
    long long to = 0;
    if (!parse_index(ctx.args.get("--to", "", ctx.mem), to)) {
        return make_usage_error("mods move: --to needs a non-negative integer", ctx);
    }

    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const bool changed =
        mol::move_mod(inst, name, static_cast<std::size_t>(to), ctx.profile_override());

    // move_mod 会对越界的目标序号做夹取，重新列一次以报告最终优先级
    std::size_t final_priority = static_cast<std::size_t>(to);
    for (const auto& m : mol::list_mods(inst, ctx.profile_override(), ctx.mem)) {
        if (same_name(m.name, name, ctx.mem)) {
            final_priority = m.priority;
            break;
        }
    }

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(ModMoveData{
        .name = mol::string(name, ctx.mem),
        .priority = final_priority,
        .changed = changed,
    });
    return r;
}

}  // namespace cli
