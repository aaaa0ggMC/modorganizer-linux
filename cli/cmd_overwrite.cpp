// overwrite capture：把游戏在农场里新建的真实文件移回 overwrite/（游戏运行中拒绝，farm_busy）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/instance.hpp"
#include "mol/overwrite.hpp"

#include "commands.hpp"

namespace cli {

Result run_overwrite_capture(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    mol::require_farm_idle(inst);
    const std::size_t n = mol::capture_overwrite(inst);
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(OverwriteCaptureData{.captured = n, .changed = n > 0});
    return r;
}

Result run_overwrite_promote(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const mol::string filter = ctx.args.get("--filter", "", ctx.mem);
    if (filter.empty()) return make_usage_error("overwrite promote: --filter is required (e.g. --filter 'cc*')", ctx);
    const bool yes = ctx.args.get_bool("--yes", false);

    std::vector<std::string> pats;
    for (std::size_t b = 0; b <= filter.size();) {
        std::size_t e = filter.find(',', b);
        if (e == mol::string::npos) e = filter.size();
        if (e > b) pats.emplace_back(filter.substr(b, e - b));
        b = e + 1;
    }
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    if (yes) mol::require_farm_idle(inst);
    const auto entries = mol::promote_overwrite(inst, pats, yes, ctx.mem);

    OverwritePromoteData d{.executed = yes, .moved = 0, .skipped = 0, .files = std::pmr::vector<PromoteRow>(ctx.mem)};
    for (const auto& e : entries) {
        d.files.push_back(PromoteRow{std::pmr::string(e.path, ctx.mem), std::pmr::string(e.dest, ctx.mem), e.skipped});
        if (e.skipped) ++d.skipped; else if (yes) ++d.moved;
    }
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    if (yes && d.moved > 0)
        r.add_warning("farm_stale", "files moved into the game directory; run `mo-linux apply` to refresh the farm", "");
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
