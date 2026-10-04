// doctor：只读体检（见 mol/doctor.hpp）。有 error 级问题时退出码 3（与 status 的漂移一致），ok 仍为 true。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/doctor.hpp"
#include "mol/game_host.hpp"
#include "mol/instance.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

Result run_doctor_cmd(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);

    // 游戏版本来自游戏层；host 缺失只是降级，不是失败。
    std::string version;
    try {
        const auto host = mol::GameHost::open();
        const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user);
        alib6::AData info(ctx.mem);
        if (info.load_from_memory(game.info_json(ctx.mem)) && info.is_object()) {
            const auto& o = info.object();
            if (auto it = o.find("version"); it != o.end()) {
                if (auto v = it.second().try_to<std::string_view>()) version = std::string(*v);
            }
        }
    } catch (const mol::Error&) {
    }

    const auto checks = mol::run_doctor(inst, version, ctx.mem);
    DoctorData d{.errors = 0, .warnings = 0, .checks = std::pmr::vector<CheckRow>(ctx.mem)};
    for (const auto& c : checks) {
        if (c.level == "error") ++d.errors;
        else if (c.level == "warn") ++d.warnings;
        CheckRow row{std::pmr::string(c.id, ctx.mem), std::pmr::string(c.level, ctx.mem), std::pmr::string(c.message, ctx.mem),
                     std::pmr::string(c.hint, ctx.mem), std::pmr::vector<std::pmr::string>(ctx.mem)};
        for (const auto& f : c.fix) row.fix.push_back(std::pmr::string(f, ctx.mem));
        d.checks.push_back(std::move(row));
    }
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = d.errors > 0 ? 3 : 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
