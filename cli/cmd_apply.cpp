// mo-linux apply 子命令：物化农场，并按批通过 EventSink 报告进度。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <algorithm>

#include "mol/instance.hpp"
#include "mol/overwrite.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

Result run_apply(Context& ctx) {
    EventSink* sink = ctx.sink;
    if (sink != nullptr) sink->start("apply");
    try {
        const mol::Instance inst =
            mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
        mol::require_farm_idle(inst);
        const mol::FarmModel model = mol::build_farm_model(inst, ctx.profile_override(), ctx.mem);
        const mol::Plan plan = mol::plan_instance(inst, model, ctx.mem);
        const std::size_t total = plan.ops.size();
        const bool needs_marker = !path_exists(std::string(inst.farm_path) + "/.mol-farm.json");

        // 空游戏目录也要能创建受管理的农场；已同步的农场避免重写 marker。
        // 整个 plan 一次执行（manifest 只读写一次），进度由 apply_farm 按批回调。
        if (total != 0 || needs_marker)
            mol::apply_instance(inst, plan, [sink](std::size_t done, std::size_t all) {
                if (sink != nullptr) sink->progress("apply", done, all);
            });
        Result r(ctx.mem);
        r.ok = true;
        r.exit_code = 0;
        r.command = ctx.command;
        r.set_data(ApplyData{
            .applied = total,
            .changed = !plan.ops.empty() || needs_marker,
            .farm_path = mol::string(inst.farm_path, ctx.mem),
        });
        // 与 plan 一致：merge 警告也进 envelope.warnings
        for (const auto& w : model.merged.warnings) {
            r.add_warning("merge_warning", std::string_view(w.message), std::string_view(w.path));
        }
        if (sink != nullptr) sink->done("apply", true);
        return r;
    } catch (...) {
        if (sink != nullptr) sink->done("apply", false);
        throw;
    }
}

}  // namespace cli
