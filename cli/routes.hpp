#pragma once
// 路由表与命令元数据（供 main 注册路由、`schema` 命令自描述、测试核对覆盖率）。
// 混用约束：本头文件会触发 `import alib6`，必须是 TU 里最后的 #include（见 cli/cmd_common.hpp）。
#include <span>
#include <string_view>

#include "commands.hpp"

namespace cli {

struct RouteEntry {
    std::string_view path;                    // alib6 路由（'/' 分层，见 Router::add_route）
    std::string_view name;                    // 规范命令名（envelope.command / 文本渲染）
    cli::CommandFn fn;
    std::span<const cli::OptionSpec> specs;   // 该命令的专有选项（空表 = 只用全局选项）
    std::size_t positionals = 0;              // 期望的位置参数个数
    std::string_view positional_name;         // 期望 1 个时的参数名（报错用）
};

inline constexpr RouteEntry kRoutes[] = {
    {"version", "version", &cli::run_version, {}, 0, ""},
    {"schema", "schema", &cli::run_schema, {}, 0, ""},
    {"docs", "docs", &cli::run_docs, {}, 1, "[TOPIC]"},
    {"next", "next", &cli::run_next, {}, 0, ""},
    {"overview", "overview", &cli::run_overview, {}, 0, ""},
    {"logs", "logs", &cli::run_logs, cli::kOptLogs, 0, ""},
    {"game/describe", "game describe", &cli::run_game_describe, {}, 1, "LUA_FILE"},
    {"game/info", "game info", &cli::run_game_info, {}, 0, ""},
    {"plugins/list", "plugins list", &cli::run_plugins_list, {}, 0, ""},
    {"plugins/enable", "plugins enable", &cli::run_plugins_enable, {}, 1, "NAME"},
    {"plugins/disable", "plugins disable", &cli::run_plugins_disable, {}, 1, "NAME"},
    {"plugins/move", "plugins move", &cli::run_plugins_move, cli::kOptPluginsMove, 1, "NAME"},
    {"plugins/sort", "plugins sort", &cli::run_plugins_sort, cli::kOptPluginsSort, 0, ""},
    {"plugins/sync", "plugins sync", &cli::run_plugins_sync, {}, 0, ""},
    {"plugins/restore", "plugins restore", &cli::run_plugins_restore, {}, 0, ""},
    {"overwrite/capture", "overwrite capture", &cli::run_overwrite_capture, {}, 0, ""},
    {"overwrite/promote", "overwrite promote", &cli::run_overwrite_promote, cli::kOptPromote, 0, ""},
    {"doctor", "doctor", &cli::run_doctor_cmd, {}, 0, ""},
    {"nexus/login", "nexus login", &cli::run_nexus_login, cli::kOptNexusLogin, 0, ""},
    {"nexus/logout", "nexus logout", &cli::run_nexus_logout, {}, 0, ""},
    {"nexus/whoami", "nexus whoami", &cli::run_nexus_whoami, {}, 0, ""},
    {"nexus/search", "nexus search", &cli::run_nexus_search, cli::kOptSearch, 1, "QUERY"},
    {"nexus/info", "nexus info", &cli::run_nexus_info, cli::kOptNexusInfo, 0, ""},
    {"nexus/install", "nexus install", &cli::run_nexus_install, cli::kOptNexusInstall, 0, ""},
    {"collection/search", "collection search", &cli::run_collection_search, cli::kOptSearch, 1, "QUERY"},
    {"nexus/files", "nexus files", &cli::run_nexus_files, cli::kOptNexusFiles, 0, ""},
    {"nexus/download", "nexus download", &cli::run_nexus_download, cli::kOptNexusDownload, 0, ""},
    {"skse/install", "skse install", &cli::run_skse_install, {}, 0, ""},
    {"terminate", "terminate", &cli::run_terminate, cli::kOptTerminate, 0, ""},
    {"fix/content-catalog", "fix content-catalog", &cli::run_fix_content_catalog, {}, 0, ""},
    {"fix/vcrun", "fix vcrun", &cli::run_fix_vcrun, cli::kOptFixVcrun, 0, ""},
    {"enb/install", "enb install", &cli::run_enb_install, cli::kOptEnbInstall, 0, ""},
    {"collection/inspect", "collection inspect", &cli::run_collection_inspect, cli::kOptCollectionInspect, 1, "COLLECTION"},
    {"collection/readme", "collection readme", &cli::run_collection_readme, cli::kOptCollectionInspect, 1, "COLLECTION"},
    {"collection/install", "collection install", &cli::run_collection_install, cli::kOptCollectionInstall, 1, "COLLECTION"},
    {"collection/status", "collection status", &cli::run_collection_status, {}, 1, "COLLECTION"},
    {"collection/verify", "collection verify", &cli::run_collection_verify, cli::kOptCollectionVerify, 1, "COLLECTION"},
    {"collection/resolve", "collection resolve", &cli::run_collection_resolve, cli::kOptCollectionResolve, 1, "COLLECTION"},
    {"wabbajack/search", "wabbajack search", &cli::run_wabbajack_search, cli::kOptWjSearch, 1, "QUERY"},
    {"wabbajack/inspect", "wabbajack inspect", &cli::run_wabbajack_inspect, {}, 1, "LIST"},
    {"wabbajack/install", "wabbajack install", &cli::run_wabbajack_install, cli::kOptWjInstall, 1, "LIST"},
    {"executables/list", "executables list", &cli::run_executables_list, {}, 0, ""},
    {"run", "run", &cli::run_run, cli::kOptRun, 0, ""},
    {"instance/init", "instance init", &cli::run_instance_init, cli::kOptInstanceInit, 0, ""},
    {"instance/default", "instance default", &cli::run_instance_default, cli::kOptInstanceDefault, 0, ""},
    {"nxm/register", "nxm register", &cli::run_nxm_register, {}, 0, ""},
    {"nxm/handle", "nxm handle", &cli::run_nxm_handle, {}, 1, "URL"},
    {"instance/show", "instance show", &cli::run_instance_show, {}, 0, ""},
    {"mods/list", "mods list", &cli::run_mods_list, {}, 0, ""},
    {"mods/enable", "mods enable", &cli::run_mods_enable, {}, 1, "NAME"},
    {"mods/disable", "mods disable", &cli::run_mods_disable, {}, 1, "NAME"},
    {"mods/install", "mods install", &cli::run_mods_install, cli::kOptModsInstall, 1, "ARCHIVE"},
    {"fomod/inspect", "fomod inspect", &cli::run_fomod_inspect, cli::kOptFomodInspect, 1, "ARCHIVE"},
    {"mods/outdated", "mods outdated", &cli::run_mods_outdated, {}, 0, ""},
    {"mods/find", "mods find", &cli::run_mods_find, cli::kOptModsFind, 1, "FILE"},
    {"mods/move", "mods move", &cli::run_mods_move, cli::kOptModsMove, 1, "NAME"},
    {"conflicts", "conflicts", &cli::run_conflicts, cli::kOptConflicts, 0, ""},
    {"script/run", "script run", &cli::run_script_run, cli::kOptScriptRun, 1, "LUA_FILE"},
    {"serve", "serve", &cli::run_serve, cli::kOptServe, 0, ""},
    {"plan", "plan", &cli::run_plan, {}, 0, ""},
    {"status", "status", &cli::run_status, {}, 0, ""},
    {"apply", "apply", &cli::run_apply, {}, 0, ""},
    {"unlink", "unlink", &cli::run_unlink, {}, 0, ""},
};


// 命令元数据：面向 Agent/GUI 的自描述。effects 是逗号分隔的标记：
//   read      只读（可能读网络）        instance  写实例目录（mods/profiles/downloads/overwrite/配置）
//   farm      改农场目录                 prefix    写用户的 Wine 前缀（含 plugins.txt 链接）
//   game_dir  写真实游戏目录             launch    启动游戏进程
//   kill      结束进程
//   network   访问 Nexus/互联网
// needs：nexus_key = 需要 Nexus API key；host = 需要 libmo-game.so；instance = 需要已有实例。
// confirm：Agent 在执行前应当向用户确认（启动游戏、不可逆地改游戏目录等）。
struct CommandMeta {
    std::string_view name;
    std::string_view summary;
    std::string_view effects;
    std::string_view needs;
    bool confirm = false;
    bool idempotent = true;
};

inline constexpr CommandMeta kMeta[] = {
    {"version", "Print the tool version", "read", "", false, true},
    {"docs", "Print the documentation compiled into the binary: no TOPIC lists them, `docs guide|agent|cli|handbook|readme` prints one in full", "read", "", false, true},
    {"schema", "Describe every command, option, effect and error code as JSON (start here)", "read", "", false, true},
    {"next", "Inspect the instance and list the recommended next commands", "read", "", false, true},
    {"overview", "Everything a dashboard needs in one call: instance, game version, doctor, next steps, mod/plugin counts, collections", "read", "", false, true},
    {"logs", "List or tail the game/SKSE/crash logs inside the Wine prefix", "read", "instance", false, true},
    {"game describe", "Validate a Lua game descriptor without the MO2 host", "read", "", false, true},
    {"game info", "Show the game plugin's view of the installation (paths, version, executables)", "read", "instance,host", false, true},
    {"instance init", "Create or update an instance; Steam paths are auto-detected", "instance", "", false, true},
    {"instance default", "Show or set the default instance (used when no -i/MOL_INSTANCE/instance in the current directory)", "instance", "", false, true},
    {"nxm register", "Register mo-linux as the browser's nxm:// link handler (writes a .desktop file, changes the MIME default)", "instance", "", true, true},
    {"nxm handle", "Download the file an nxm:// link points to into the default instance and hand it to any waiting collection", "instance,network", "instance,nexus_key", false, true},
    {"instance show", "Show the resolved instance paths and configuration", "read", "instance", false, true},
    {"mods list", "List mods in the profile (low to high priority)", "read", "instance", false, true},
    {"mods enable", "Enable a mod in the profile", "instance", "instance", false, true},
    {"mods disable", "Disable a mod in the profile", "instance", "instance", false, true},
    {"mods move", "Change a mod's priority", "instance", "instance", false, true},
    {"mods install", "Install a local archive as a mod (FOMOD aware)", "instance", "instance", false, false},
    {"mods outdated", "Compare installed Nexus mods' versions with the site (one batched request)", "network", "instance,nexus_key", false, true},
    {"conflicts", "List files provided by several mods and who wins", "read", "instance", false, true},
    {"script run", "Run a Lua install script inside its sandboxed virtual root (fs/archive/net/proc/state; HTTP namespace for progress)", "instance,launch,network", "instance", true, false},
    {"serve", "Run the background script service: one HTTP port + global namespace registry for every instance (dockerd style)", "read", "", false, true},
    {"plan", "Show what `apply` would change in the farm", "read", "instance", false, true},
    {"status", "Exit 3 if the farm differs from the desired state", "read", "instance", false, true},
    {"apply", "Make the farm match the profile (symlinks only)", "farm", "instance", false, true},
    {"unlink", "Remove the farm", "farm", "instance", false, true},
    {"doctor", "Health check with a machine-readable fix for each problem", "read", "instance", false, true},
    {"plugins list", "Plugin load order, masters and problems", "read", "instance", false, true},
    {"plugins enable", "Enable a plugin", "instance", "instance", false, true},
    {"plugins disable", "Disable a plugin", "instance", "instance", false, true},
    {"plugins move", "Move a plugin in the load order", "instance", "instance", false, true},
    {"plugins sort", "Sort plugins so masters come first; --loot also applies the LOOT masterlist rules and groups", "instance,network", "instance", false, true},
    {"plugins sync", "Link the profile's plugins.txt into the Wine prefix", "instance,prefix", "instance,host", false, true},
    {"overwrite capture", "Move files the game created inside the farm into overwrite/", "instance,farm", "instance", false, true},
    {"overwrite promote", "Move overwrite files into the REAL game Data directory (preview unless --yes)", "instance,game_dir", "instance", true, false},
    {"executables list", "List the executables registered in the instance (SKSE, xEdit, tools…)", "read", "instance", false, true},
    {"run", "Apply, sync plugins and start the game, SKSE or a registered tool (--title) through Proton", "instance,farm,prefix,launch", "instance,host", true, false},
    {"fomod inspect", "Show a FOMOD installer's steps/groups/plugins for given choices", "read", "instance", false, true},
    {"nexus login", "Store a Nexus API key read from stdin or a file (validated first)", "network", "", false, true},
    {"nexus logout", "Delete the stored Nexus API key", "read", "", false, true},
    {"nexus whoami", "Show the Nexus account tied to the key", "network", "nexus_key", false, true},
    {"nexus search", "Search Nexus mods for this game", "network", "instance,nexus_key", false, true},
    {"nexus info", "Mod details and its declared requirements", "network", "instance,nexus_key", false, true},
    {"nexus files", "List the files of a Nexus mod", "network", "instance,nexus_key", false, true},
    {"nexus download", "Download a Nexus file into downloads/", "instance,network", "instance,nexus_key", false, true},
    {"nexus install", "Download and install a Nexus mod (optionally with requirements)", "instance,network", "instance,nexus_key", false, true},
    {"skse install", "Install the SKSE64 build that matches the game version", "instance,network", "instance,nexus_key,host", false, true},
    {"collection verify", "Recompute what each installed collection mod should contain with the current installer logic and list the ones whose files differ (--fix marks FOMOD ones for reinstall)", "read,instance", "instance", false, true},
    {"mods find", "Find which mods (and, with --archives, which downloaded archives) contain a file, e.g. a missing master", "read", "instance", false, true},
    {"plugins restore", "Put back the plugin list mo-linux last wrote (after the game rewrote plugins.txt and disabled plugins)", "instance", "instance", false, true},
    {"terminate", "End every process using this instance (game, leftover Wine processes such as steam.exe steam://run, wineserver); fixes farm_busy/wine_busy", "kill", "instance", true, true},
    {"fix content-catalog", "Move aside a ContentCatalog.txt the game version cannot parse (it is rebuilt on the next start)", "prefix", "instance", false, true},
    {"fix vcrun", "Install the current VC++ 2015-2022 runtime into the prefix (needed by most SKSE plugins)", "prefix,network,launch", "instance", true, true},
    {"enb install", "Install the ENB binaries (d3d11.dll, d3dcompiler_46e.dll) from an enbdev.com zip as a root-folder mod", "instance", "instance", false, true},
    {"wabbajack search", "Search the Wabbajack gallery (official repositories) for mod lists", "network", "", false, true},
    {"wabbajack inspect", "Analyse a mod list: sources, directive types and how much of it mo-linux can install", "network", "", false, true},
    {"wabbajack install", "Rebuild the mod list's instance in -i DIR; resumable, exit 4 when it needs input", "instance,network", "nexus_key", true, true},
    {"collection search", "Search Nexus collections for this game", "network", "instance,nexus_key", false, true},
    {"collection inspect", "Show a collection's mods, order, status and its page description (README)", "network", "instance,nexus_key", false, true},
    {"collection readme", "Show the collection page's description (the author's README: requirements, downgrade steps, options) as Markdown; read it before installing", "network", "nexus_key", false, true},
    {"collection install", "Install a collection; resumable, returns exit 4 when it needs input", "instance,network", "instance,nexus_key", false, true},
    {"collection status", "Show the stored state of a collection install", "read", "instance", false, true},
    {"collection resolve", "Record a decision for a pending collection mod", "instance,network", "instance", false, true},
};

}  // namespace cli
