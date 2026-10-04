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

}  // namespace cli
