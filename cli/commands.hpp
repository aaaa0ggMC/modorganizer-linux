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
Result run_plugins_list(Context&);
Result run_plugins_enable(Context&);
Result run_plugins_disable(Context&);
Result run_plugins_move(Context&);
Result run_plugins_sort(Context&);
Result run_run(Context&);
Result run_instance_default(Context&);
Result run_nxm_register(Context&);
Result run_nxm_handle(Context&);
Result run_wabbajack_search(Context&);
Result run_wabbajack_inspect(Context&);
Result run_wabbajack_install(Context&);
Result run_schema(Context&);
Result run_next(Context&);
Result run_logs(Context&);
Result run_collection_inspect(Context&);
Result run_collection_install(Context&);
Result run_collection_status(Context&);
Result run_collection_resolve(Context&);
Result run_fomod_inspect(Context&);
Result run_skse_install(Context&);
Result run_nexus_login(Context&);
Result run_nexus_logout(Context&);
Result run_nexus_whoami(Context&);
Result run_nexus_files(Context&);
Result run_nexus_download(Context&);
Result run_nexus_search(Context&);
Result run_nexus_info(Context&);
Result run_nexus_install(Context&);
Result run_collection_search(Context&);
Result run_doctor_cmd(Context&);
Result run_overwrite_capture(Context&);
Result run_overwrite_promote(Context&);
Result run_instance_init(Context&);
Result run_instance_show(Context&);
Result run_mods_list(Context&);
Result run_mods_enable(Context&);
Result run_mods_disable(Context&);
Result run_mods_move(Context&);
Result run_mods_install(Context&);
Result run_mods_outdated(Context&);
Result run_conflicts(Context&);
Result run_plan(Context&);
Result run_status(Context&);
Result run_apply(Context&);
Result run_unlink(Context&);

}  // namespace cli
