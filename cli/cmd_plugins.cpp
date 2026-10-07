// plugins sync：把游戏层 mappings() 物化为符号链接（见 mol/plugins_sync.hpp）。会写 profile 与前缀 AppData，不启动游戏。
#include <fstream>
#include <iterator>

#include "mol/game_host.hpp"
#include "mol/instance.hpp"
#include "mol/loot.hpp"
#include "mol/overwrite.hpp"
#include "mol/plugins.hpp"
#include "mol/plugins_sync.hpp"

#include "commands.hpp"

namespace cli {

Result run_plugins_sync(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const auto host = mol::GameHost::open();  // 先于 Game 声明，后销毁
    const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user);
    const auto rep = mol::sync_plugins(inst, game, ctx.mem);

    PluginsSyncData d{.profile = std::pmr::string(rep.profile, ctx.mem),
                      .initialized_profile = rep.initialized_profile,
                      .changed = rep.changed,
                      .entries = std::pmr::vector<SyncRow>(ctx.mem)};
    for (const auto& e : rep.entries)
        d.entries.push_back(SyncRow{std::pmr::string(e.source, ctx.mem), std::pmr::string(e.destination, ctx.mem),
                                    std::pmr::string(e.action, ctx.mem)});
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

namespace {

std::vector<mol::string> forced_list(Context& ctx, const mol::Instance& inst) {
    std::vector<mol::string> out;
    for (const auto& n : forced_plugin_names(ctx, inst)) out.emplace_back(n, ctx.mem);
    return out;  // 空 → load_plugins 用后备清单
}

PluginsListData to_data(Context& ctx, const mol::PluginList& list, bool changed, std::string_view profile) {
    PluginsListData d{.profile = std::pmr::string(profile, ctx.mem), .changed = changed, .sorted_with = std::pmr::string(ctx.mem), .plugins = std::pmr::vector<PluginRowData>(ctx.mem),
                      .issues = std::pmr::vector<MasterIssueRow>(ctx.mem)};
    std::size_t i = 0;
    for (const auto& r : list.rows) {
        PluginRowData row{.name = std::pmr::string(r.name, ctx.mem), .index = i++, .enabled = r.enabled, .forced = r.forced, .master = r.master,
                          .light = r.light, .source = std::pmr::string(r.source, ctx.mem), .masters = std::pmr::vector<std::pmr::string>(ctx.mem)};
        for (const auto& m : r.masters) row.masters.push_back(std::pmr::string(m, ctx.mem));
        d.plugins.push_back(std::move(row));
    }
    for (const auto& is : mol::check_masters(list, ctx.mem))
        d.issues.push_back(MasterIssueRow{std::pmr::string(is.plugin, ctx.mem), std::pmr::string(is.master, ctx.mem), std::pmr::string(is.kind, ctx.mem)});
    return d;
}

Result finish(Context& ctx, const mol::Instance& inst, const mol::PluginList& list, bool changed, std::string_view sorted_with = {},
              std::int64_t rules = 0, std::int64_t grouped = 0) {
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    auto d = to_data(ctx, list, changed, inst.cfg.profile);
    d.sorted_with = std::pmr::string(sorted_with, ctx.mem);
    d.rules_applied = rules;
    d.grouped = grouped;
    r.set_data(std::move(d));
    return r;
}

}  // namespace

Result run_plugins_list(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const auto forced = forced_list(ctx, inst);
    const auto list = mol::load_plugins(inst, forced, ctx.profile_override(), ctx.mem);
    return finish(ctx, inst, list, false);
}

Result run_plugins_enable(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    auto list = mol::load_plugins(inst, forced_list(ctx, inst), ctx.profile_override(), ctx.mem);
    const bool changed = mol::plugin_set_enabled(list, ctx.args.positionals.front(), true);
    mol::save_plugins(inst, list, ctx.profile_override());  // 总是写：把新发现的插件与规范化后的顺序固化下来
    return finish(ctx, inst, list, changed);
}

Result run_plugins_disable(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    auto list = mol::load_plugins(inst, forced_list(ctx, inst), ctx.profile_override(), ctx.mem);
    const bool changed = mol::plugin_set_enabled(list, ctx.args.positionals.front(), false);
    mol::save_plugins(inst, list, ctx.profile_override());
    return finish(ctx, inst, list, changed);
}

Result run_plugins_move(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const mol::string to_s = ctx.args.get("--to", "", ctx.mem);
    std::size_t to = 0;
    if (to_s.empty() || to_s.find_first_not_of("0123456789") != mol::string::npos || to_s.size() > 9)
        return make_usage_error("plugins move: --to needs a non-negative integer", ctx);
    to = static_cast<std::size_t>(std::stoul(std::string(to_s)));
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    auto list = mol::load_plugins(inst, forced_list(ctx, inst), ctx.profile_override(), ctx.mem);
    const bool changed = mol::plugin_move(list, ctx.args.positionals.front(), to);
    mol::save_plugins(inst, list, ctx.profile_override());
    return finish(ctx, inst, list, changed);
}

Result run_plugins_restore(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    mol::require_farm_idle(inst);
    if (!mol::restore_plugins_snapshot(inst, ctx.profile_override()))
        throw mol::Error("mod_not_found", "no plugins.txt.mol-last-good in this profile yet (mo-linux has not written the list since this feature was added)");
    auto list = mol::load_plugins(inst, forced_list(ctx, inst), ctx.profile_override(), ctx.mem);
    mol::save_plugins(inst, list, ctx.profile_override());
    return finish(ctx, inst, list, true, "masters", 0, 0);
}

Result run_plugins_sort(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    auto list = mol::load_plugins(inst, forced_list(ctx, inst), ctx.profile_override(), ctx.mem);
    const mol::string mlfile = ctx.args.get("--masterlist", "", ctx.mem);
    const bool use_loot = ctx.args.get_bool("--loot", false) || !mlfile.empty();
    bool changed = false;
    mol::loot::SortReport rep;
    if (use_loot) {
        std::string yaml;
        if (!mlfile.empty()) {
            std::ifstream in{std::string(mlfile), std::ios::binary};
            if (!in) throw mol::Error("io_error", "cannot read the masterlist file", std::string(mlfile));
            yaml.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        } else {
            yaml = mol::loot::fetch_masterlist({}, {}, ctx.args.get_bool("--refresh", false));
        }
        const auto ml = mol::loot::parse_masterlist(yaml);
        rep = mol::loot::sort_with_masterlist(list, ml);
        changed = rep.changed;
    }
    changed = mol::plugin_sort_by_masters(list) || changed;  // 最后兜底保证 masters 在前
    mol::save_plugins(inst, list, ctx.profile_override());
    Result r = finish(ctx, inst, list, changed, use_loot ? "loot" : "masters", static_cast<std::int64_t>(rep.rules_applied), static_cast<std::int64_t>(rep.grouped));
    for (const auto& c : rep.cycles) r.add_warning("loot_note", c, "");
    return r;
}

}  // namespace cli
