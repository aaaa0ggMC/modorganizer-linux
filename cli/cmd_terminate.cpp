// terminate：结束占用实例的所有进程（见 mol/terminate.hpp）。farm_busy / wine_busy 的通用解法。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <string>

#include "mol/instance.hpp"
#include "mol/terminate.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

Result run_terminate(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const bool dry = ctx.args.get_bool("--dry-run", false);
    std::int64_t secs = 10;
    if (ctx.args.has("--timeout")) {
        const std::string t(ctx.args.get("--timeout", "", ctx.mem));
        if (t.empty() || t.size() > 4 || t.find_first_not_of("0123456789") != std::string::npos) return make_usage_error("terminate: --timeout needs seconds (0-9999)", ctx);
        secs = std::stoll(t);
    }
    const mol::TerminateResult res = mol::terminate_instance(inst, static_cast<int>(secs * 1000), dry);
    TerminateData d{.dry_run = dry, .processes = std::pmr::vector<TerminateProcRow>(ctx.mem), .terminated = std::pmr::vector<std::int64_t>(ctx.mem),
                    .killed = std::pmr::vector<std::int64_t>(ctx.mem), .remaining = std::pmr::vector<std::int64_t>(ctx.mem), .wineserver_killed = res.wineserver_killed};
    for (const auto& p : res.found) d.processes.push_back(TerminateProcRow{p.pid, std::pmr::string(p.command, ctx.mem), std::pmr::string(p.reason, ctx.mem)});
    for (int p : res.terminated) d.terminated.push_back(p);
    for (int p : res.killed) d.killed.push_back(p);
    for (int p : res.remaining) d.remaining.push_back(p);
    Result r = make_ok(ctx);
    if (!res.remaining.empty()) {
        r.exit_code = 1;
        r.add_warning("still_running", std::to_string(res.remaining.size()) + " process(es) survived SIGKILL", "");
    }
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
