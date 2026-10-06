// doctor：只读体检（见 mol/doctor.hpp）。有 error 级问题时退出码 3（与 status 的漂移一致），ok 仍为 true。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/doctor.hpp"
#include "mol/instance.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

DoctorData doctor_data(Context& ctx, const mol::vector<mol::Check>& checks) {
    DoctorData d{.errors = 0, .warnings = 0, .checks = std::pmr::vector<CheckRow>(ctx.mem)};
    for (const auto& c : checks) {
        if (c.level == "error") ++d.errors;
        else if (c.level == "warn") ++d.warnings;
        CheckRow row{std::pmr::string(c.id, ctx.mem), std::pmr::string(c.level, ctx.mem), std::pmr::string(c.message, ctx.mem),
                     std::pmr::string(c.hint, ctx.mem), std::pmr::vector<std::pmr::string>(ctx.mem)};
        for (const auto& f : c.fix) row.fix.push_back(std::pmr::string(f, ctx.mem));
        d.checks.push_back(std::move(row));
    }
    return d;
}

Result run_doctor_cmd(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    // 游戏版本来自游戏层；host 缺失只是降级，不是失败。
    const std::string version = game_info_string(ctx, inst, "version");
    DoctorData d = doctor_data(ctx, mol::run_doctor(inst, version, ctx.mem));
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = d.errors > 0 ? 3 : 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
