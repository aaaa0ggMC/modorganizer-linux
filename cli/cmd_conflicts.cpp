// mo-linux conflicts 子命令（层名 = mod 名 / <game> / <overwrite>）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/casefold.hpp"
#include "mol/instance.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

namespace {

// 层下标 → 层名（越界时给个占位，绝不越界读）
std::string_view layer_name(const mol::FarmModel& model, std::size_t index) {
    if (index < model.layer_names.size()) return std::string_view(model.layer_names[index]);
    return std::string_view("<unknown>");
}

// 把 merge 警告原样搬进 envelope.warnings（code = merge_warning）
void add_merge_warnings(Result& r, const mol::FarmModel& model) {
    for (const auto& w : model.merged.warnings) {
        r.add_warning("merge_warning", std::string_view(w.message), std::string_view(w.path));
    }
}

}  // namespace

Result run_conflicts(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);

    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::FarmModel model = mol::build_farm_model(inst, ctx.profile_override(), ctx.mem);

    const mol::string mod = ctx.args.get("--mod", "", ctx.mem);
    const bool filter = !mod.empty();
    const mol::string mod_fold = filter ? mol::casefold(mod, ctx.mem) : mol::string(ctx.mem);

    ConflictsData d{
        .conflicts = mol::vector<ConflictRow>(ctx.mem),
        .count = 0,
    };
    for (const auto& c : model.merged.conflicts) {
        const std::string_view winner = layer_name(model, c.winner);
        bool involves = false;
        if (filter && mol::casefold(winner, ctx.mem) == mod_fold) involves = true;
        for (const auto loser : c.losers) {
            if (filter && mol::casefold(layer_name(model, loser), ctx.mem) == mod_fold) {
                involves = true;
                break;
            }
        }
        if (filter && !involves) continue;

        ConflictRow row{
            .path = mol::string(c.path, ctx.mem),
            .winner = mol::string(winner, ctx.mem),
            .losers = mol::vector<mol::string>(ctx.mem),
        };
        row.losers.reserve(c.losers.size());
        for (const auto loser : c.losers) {
            row.losers.push_back(mol::string(layer_name(model, loser), ctx.mem));
        }
        d.conflicts.push_back(std::move(row));
        ++d.count;
    }

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(d);
    add_merge_warnings(r, model);
    return r;
}

}  // namespace cli
