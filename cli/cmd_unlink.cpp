// mo-linux unlink 子命令：删除农场（幂等：不存在 → removed:false，不是错误）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/instance.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

Result run_unlink(Context& ctx) {
    const mol::Instance inst =
        mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);

    bool removed = false;
    if (path_exists(inst.farm_path)) {
        try {
            mol::remove_farm(inst.farm_path);
            removed = true;
        } catch (const mol::Error&) {
            throw;  // 已经是带 code 的错误
        } catch (const std::exception& e) {
            // remove_farm 对我们的 marker 缺失/非空目录抛 runtime_error：拒绝碰用户目录
            const std::string msg = e.what();
            if (msg.find("not a farm") != std::string::npos ||
                msg.find("not empty") != std::string::npos) {
                throw mol::Error("farm_not_owned", msg, std::string(inst.farm_path));
            }
            throw;
        }
    }

    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(UnlinkData{
        .removed = removed,
        .farm_path = mol::string(inst.farm_path, ctx.mem),
    });
    return r;
}

}  // namespace cli
