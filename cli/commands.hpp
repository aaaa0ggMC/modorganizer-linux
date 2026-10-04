#pragma once
// 子命令入口声明。
//
// 包含 cli/cmd_common.hpp（内有 `import alib6;`）→ 本头文件必须是 TU 中最后的 #include
// （混用约束详见 cli/cmd_common.hpp 文件头）。
#include "cmd_common.hpp"

namespace cli {

Result run_version(Context&);
Result run_game_info(Context&);
Result run_plugins_sync(Context&);
Result run_run(Context&);
Result run_doctor_cmd(Context&);
Result run_overwrite_capture(Context&);
Result run_overwrite_promote(Context&);
Result run_instance_init(Context&);
Result run_instance_show(Context&);
Result run_mods_list(Context&);
Result run_mods_enable(Context&);
Result run_mods_disable(Context&);
Result run_mods_move(Context&);
Result run_conflicts(Context&);
Result run_plan(Context&);
Result run_status(Context&);
Result run_apply(Context&);
Result run_unlink(Context&);

}  // namespace cli
