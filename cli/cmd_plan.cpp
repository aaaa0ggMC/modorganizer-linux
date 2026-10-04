// mo-linux plan / status 子命令（只读）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/instance.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

namespace {

const char* op_kind_name(mol::OpKind kind) {
    switch (kind) {
        case mol::OpKind::Mkdir: return "mkdir";
        case mol::OpKind::Link: return "link";
        case mol::OpKind::Relink: return "relink";
        case mol::OpKind::Remove: return "remove";
        case mol::OpKind::Rmdir: return "rmdir";
    }
    return "link";
}

OpCounts count_ops(const mol::vector<mol::Op>& ops) {
    OpCounts c;
    for (const auto& op : ops) {
        switch (op.kind) {
            case mol::OpKind::Mkdir: ++c.mkdir; break;
            case mol::OpKind::Link: ++c.link; break;
            case mol::OpKind::Relink: ++c.relink; break;
            case mol::OpKind::Remove: ++c.remove; break;
            case mol::OpKind::Rmdir: ++c.rmdir; break;
        }
    }
    return c;
}

void add_merge_warnings(Result& r, const mol::FarmModel& model) {
    for (const auto& w : model.merged.warnings) {
        r.add_warning("merge_warning", std::string_view(w.message), std::string_view(w.path));
    }
}

}  // namespace

Result run_plan(Context& ctx) {
    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::FarmModel model = mol::build_farm_model(inst, ctx.profile_override(), ctx.mem);
    const mol::Plan plan = mol::plan_instance(inst, model, ctx.mem);

    PlanData d{
        .ops = mol::vector<OpRow>(ctx.mem),
        .count = plan.ops.size(),
        .counts = count_ops(plan.ops),
        .warnings = model.merged.warnings.size(),
    };
    d.ops.reserve(plan.ops.size());
    for (const auto& op : plan.ops) {
        d.ops.push_back(OpRow{
            .kind = mol::string(op_kind_name(op.kind), ctx.mem),
            .path = mol::string(op.path, ctx.mem),
            .target = mol::string(op.target, ctx.mem),
        });
    }

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(d);
    add_merge_warnings(r, model);
    return r;
}

Result run_status(Context& ctx) {
    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::FarmModel model = mol::build_farm_model(inst, ctx.profile_override(), ctx.mem);
    const mol::Plan plan = mol::plan_instance(inst, model, ctx.mem);

    const bool in_sync = plan.ops.empty();
    Result r(ctx.mem);
    r.ok = true;
    // 漂移时 ok 仍为 true，但退出码为 3
    r.exit_code = in_sync ? 0 : 3;
    r.command = ctx.command;
    r.set_data(StatusData{
        .in_sync = in_sync,
        .pending = plan.ops.size(),
        .farm_path = mol::string(inst.farm_path, ctx.mem),
        .farm_exists = path_exists(inst.farm_path),
    });
    add_merge_warnings(r, model);
    return r;
}

}  // namespace cli
