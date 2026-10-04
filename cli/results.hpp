#pragma once
// mo-linux CLI 的命令结果结构体。
//
// 字段名与 docs/CLI.md 的 data 表严格一致（GUI 直接依赖）。
// 序列化走 alib6 C++26 反射：to_adata(struct) → AData → JSON（紧凑 + 键排序），
// 见 cli/cmd_common.hpp / cli/output.cpp。
//
// 约定（来自 mol/pmr.hpp 的项目约定 + alib6 反射的限制）：
//   * 字符串成员一律 std::pmr::string；容器一律 std::pmr::vector；
//   * 不用 string_view 成员、不用原始指针（反射不支持/会悬垂）；
//   * 这些结构体是聚合体：总是以聚合初始化 + 显式 mem 构造，例如
//       StatusData d{.farm_path = mol::string(p, mem), ...};
//     避免默认构造（那会悄悄落到全局 new）。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mol/pmr.hpp"

namespace cli {

// envelope 的 warnings/errors 条目
struct Err {
    std::pmr::string code;
    std::pmr::string message;
    std::pmr::string path;  // 可空
    std::pmr::string hint;  // 可空：给人/Agent 的下一步建议（可执行命令用反引号包起来）
};

// ---- version --------------------------------------------------------------
struct VersionData {
    std::pmr::string name;
    std::pmr::string version;
};

// ---- instance -------------------------------------------------------------
// mo-linux.json 的内容（game…steam_root 九个字段，与 InstanceConfig 对应）
struct ConfigData {
    std::pmr::string game;
    std::pmr::string game_dir;
    std::pmr::string prefix;
    std::pmr::string prefix_user;
    std::pmr::string profile;
    std::pmr::string farm_dir;
    std::pmr::string runner_kind;
    std::pmr::string proton_path;
    std::pmr::string steam_root;
};

struct InstanceInitData {
    std::pmr::string root;
    bool changed = false;
    ConfigData config;
};

struct InstanceShowData {
    std::pmr::string root;
    std::pmr::string mods_dir;
    std::pmr::string profiles_dir;
    std::pmr::string downloads_dir;
    std::pmr::string overwrite_dir;
    std::pmr::string farm_path;
    ConfigData config;
};

// ---- mods -----------------------------------------------------------------
struct ModRow {
    std::pmr::string name;
    bool enabled = false;
    bool separator = false;
    bool exists = false;
    bool root = false;  // 根目录型 mod（映射到农场根而非 Data/）
    std::int64_t nexus_id = 0;  // meta.ini 的 modid（0=未知）
    std::size_t priority = 0;
    std::pmr::string path;  // 分隔符为空
};

struct ModsListData {
    std::pmr::string profile;
    std::pmr::vector<ModRow> mods;  // 低→高优先级
};

struct ModToggleData {
    std::pmr::string name;
    bool enabled = false;
    bool changed = false;
};

struct ModMoveData {
    std::pmr::string name;
    std::size_t priority = 0;
    bool changed = false;
};

struct ModInstallData {
    std::pmr::string name;
    std::pmr::string path;
    bool root = false;
    std::size_t files = 0;
    bool fomod = false;
    std::pmr::vector<std::pmr::string> missing;  // FOMOD 引用但压缩包里没有的源
};

// ---- conflicts ------------------------------------------------------------
struct ConflictRow {
    std::pmr::string path;
    std::pmr::string winner;  // 层名
    std::pmr::vector<std::pmr::string> losers;
};

struct ConflictsData {
    std::pmr::vector<ConflictRow> conflicts;
    std::size_t count = 0;
};

// ---- plugins sync -----------------------------------------------------------
struct SyncRow {
    std::pmr::string source;
    std::pmr::string destination;
    std::pmr::string action;  // ok|link|relink|backup+link|skip-missing-source
};

struct PluginsSyncData {
    std::pmr::string profile;
    bool initialized_profile = false;
    bool changed = false;
    std::pmr::vector<SyncRow> entries;
};

// ---- overwrite capture ------------------------------------------------------
struct OverwriteCaptureData {
    std::size_t captured = 0;
    bool changed = false;
};

struct PromoteRow {
    std::pmr::string path;
    std::pmr::string dest;
    bool skipped = false;
};
struct OverwritePromoteData {
    bool executed = false;
    std::size_t moved = 0;
    std::size_t skipped = 0;
    std::pmr::vector<PromoteRow> files;
};

// ---- doctor -----------------------------------------------------------------
struct CheckRow {
    std::pmr::string id;
    std::pmr::string level;  // ok|warn|error
    std::pmr::string message;
    std::pmr::string hint;
    std::pmr::vector<std::pmr::string> fix;  // 可执行的修复命令 argv（不含程序名），空 = 需要人处理
};
struct DoctorData {
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::pmr::vector<CheckRow> checks;
};

// ---- nexus ------------------------------------------------------------------
struct NexusUserData {
    std::pmr::string name;
    std::int64_t user_id = 0;
    bool is_premium = false;
    bool is_supporter = false;
    std::pmr::string key_path;  // login 时给出（key 本身永不输出）
};
struct NexusLogoutData {
    bool removed = false;
};
struct NexusFileRow {
    std::int64_t file_id = 0;
    std::pmr::string name;
    std::pmr::string file_name;
    std::pmr::string version;
    std::pmr::string category;
    std::int64_t size_kb = 0;
    bool is_primary = false;
};
struct NexusFilesData {
    std::pmr::string game;
    std::int64_t mod_id = 0;
    std::pmr::vector<NexusFileRow> files;
};
struct NexusDownloadData {
    std::pmr::string path;
    std::uint64_t size = 0;
    std::pmr::string game;
    std::int64_t mod_id = 0;
    std::int64_t file_id = 0;
};

struct SkseInstallData {
    std::pmr::string game_version;
    std::pmr::string runtime_dll;
    bool installed = false;
    std::pmr::string mod_name;
    std::pmr::string file_name;
    std::int64_t file_id = 0;
    bool downloaded = false;
};

struct FomodPluginRow {
    std::pmr::string name;
    std::pmr::string description;
    std::pmr::string image;
    std::pmr::string type;  // Required|Optional|Recommended|NotUsable|CouldBeUsable（按当前选择求值）
    bool selected = false;
};
struct FomodGroupRow {
    std::pmr::string name;
    std::pmr::string type;  // SelectExactlyOne 等
    bool explicit_choice = false;
    std::pmr::vector<FomodPluginRow> plugins;
};
struct FomodStepRow {
    std::pmr::string name;
    bool visible = true;
    std::pmr::vector<FomodGroupRow> groups;
};
struct FomodFileRow {
    std::pmr::string source;
    std::pmr::string destination;
    bool folder = false;
    std::int64_t priority = 0;
};
struct FomodInspectData {
    bool has_fomod = false;
    std::pmr::string module_name;
    std::pmr::vector<FomodStepRow> steps;
    std::pmr::vector<FomodFileRow> files;  // 按当前选择（未给的组用默认）将安装的文件，已排序
};

struct PluginRowData {
    std::pmr::string name;
    std::size_t index = 0;
    bool enabled = false;
    bool forced = false;
    bool master = false;
    bool light = false;
    std::pmr::string source;
    std::pmr::vector<std::pmr::string> masters;
};
struct MasterIssueRow {
    std::pmr::string plugin;
    std::pmr::string master;
    std::pmr::string kind;  // missing|disabled|after
};
struct PluginsListData {
    std::pmr::string profile;
    bool changed = false;  // 本次命令是否改动了 plugins.txt/loadorder.txt（list 恒为 false）
    std::pmr::string sorted_with;  // sort 时：masters | loot
    std::int64_t rules_applied = 0; // loot：命中的 after/req 边数
    std::int64_t grouped = 0;       // loot：有分组的插件数
    std::pmr::vector<PluginRowData> plugins;
    std::pmr::vector<MasterIssueRow> issues;
};

// ---- collection -------------------------------------------------------------
struct CollectionModRow {
    std::pmr::string key;
    std::pmr::string name;
    std::pmr::string version;
    bool optional = false;
    std::pmr::string source_type;
    std::int64_t mod_id = 0;
    std::int64_t file_id = 0;
    bool has_fomod_choices = false;
    bool has_patches = false;
    std::pmr::string status;  // 无状态时为 "new"
};
struct CollectionInspectData {
    std::pmr::string name;
    std::pmr::string slug;
    std::pmr::string author;
    std::pmr::string domain;
    std::int64_t revision = 0;
    std::pmr::vector<std::pmr::string> game_versions;
    std::pmr::string game_version;  // 本机游戏的版本（取不到为空）
    std::int64_t mod_count = 0;
    std::int64_t total_size = 0;
    std::int64_t plugin_count = 0;
    std::int64_t rule_count = 0;
    std::pmr::string install_instructions;
    std::pmr::vector<CollectionModRow> mods;  // 按安装顺序
};
struct CollectionOutcomeRow {
    std::pmr::string key;
    std::pmr::string name;
    std::pmr::string status;  // installed|skipped|pending|failed
    std::pmr::string mod_dir;
    std::pmr::string note;
};
struct CollectionPendingRow {
    std::pmr::string key;
    std::pmr::string name;
    std::pmr::string kind;  // manual_download|fomod_choices|unsupported
    std::pmr::string detail;
    std::pmr::string url;
};
struct CollectionInstallData {
    std::pmr::string name;
    std::pmr::string slug;
    std::int64_t revision = 0;
    std::pmr::string profile;
    std::pmr::string status;  // complete|incomplete
    std::int64_t installed = 0;
    std::int64_t skipped = 0;
    std::int64_t failed = 0;
    std::int64_t plugins_applied = 0;
    std::pmr::vector<CollectionOutcomeRow> mods;
    std::pmr::vector<CollectionPendingRow> pending;
    std::pmr::vector<std::pmr::string> notes;
};
struct CollectionResolveData {
    std::pmr::string key;
    std::pmr::string recorded;  // skip|fomod_choices|fomod_defaults|archive
    std::pmr::string archive;
};

struct NexusModRow {
    std::int64_t mod_id = 0;
    std::pmr::string name;
    std::pmr::string author;
    std::pmr::string summary;
    std::pmr::string version;
    std::pmr::string updated_at;
    std::int64_t endorsements = 0;
    std::int64_t downloads = 0;
    bool installed = false;  // 本实例里已有来自该 mod 的安装
};
struct NexusSearchData {
    std::pmr::string game;
    std::pmr::string query;
    std::pmr::string sort;
    std::int64_t total = 0;
    std::pmr::vector<NexusModRow> mods;
};
struct NexusRequirementRow {
    std::int64_t mod_id = 0;
    std::pmr::string name;
    bool external = false;
    std::pmr::string url;
    std::pmr::string notes;
    bool installed = false;
};
struct NexusInfoData {
    NexusModRow mod;
    std::pmr::string category;
    std::pmr::vector<NexusRequirementRow> requirements;
    std::pmr::vector<std::pmr::string> dlc_requirements;
};
struct NexusCollectionRow {
    std::pmr::string slug;
    std::pmr::string name;
    std::pmr::string summary;
    std::int64_t endorsements = 0;
    std::int64_t downloads = 0;
    std::int64_t revision = 0;
    std::int64_t mod_count = 0;
    std::int64_t total_size = 0;
};
struct CollectionSearchData {
    std::pmr::string game;
    std::pmr::string query;
    std::pmr::string sort;
    std::int64_t total = 0;
    std::pmr::vector<NexusCollectionRow> collections;
};
struct NexusInstalledRow {
    std::int64_t mod_id = 0;
    std::pmr::string name;
    std::pmr::string status;  // installed|already_installed|pending|failed
    std::pmr::string mod_dir;
    std::pmr::string note;
};
struct NexusInstallData {
    std::pmr::string status;  // complete|incomplete
    std::pmr::vector<NexusInstalledRow> mods;       // 本次涉及的 mod（含自动装的前置）
    std::pmr::vector<CollectionPendingRow> pending; // 需要人介入的项（kind 同 collection）
    std::pmr::vector<NexusRequirementRow> external_requirements;
    std::pmr::vector<std::pmr::string> dlc_requirements;
};

// ---- next / logs ------------------------------------------------------------
struct NextStep {
    std::pmr::string id;       // 稳定标识，如 "instance.init"、"skse.install"
    std::pmr::string why;
    std::pmr::vector<std::pmr::string> command;  // mo-linux 子命令 argv（不含程序名）；空 = 需要人手工处理
    std::pmr::string effects;  // 同 schema 的 effects
    bool blocking = false;     // true = 不做完就还没就绪
    bool needs_human = false;  // true = Agent 不能自己完成（要向用户索取信息/让用户操作）
    bool confirm = false;      // true = 执行前应向用户确认
};
struct NextData {
    bool ready = false;        // 没有 blocking 步骤
    std::pmr::string instance; // 解析出的实例目录
    std::pmr::vector<NextStep> steps;
};
struct LogFileRow {
    std::pmr::string name;
    std::int64_t size = 0;
    std::int64_t modified = 0;  // unix 秒
};
struct LogsData {
    std::pmr::string dir;
    std::pmr::vector<LogFileRow> files;  // 按修改时间新→旧
    std::pmr::string name;               // 指定了 NAME 时
    std::pmr::string tail;               // 该文件的最后 N 行
};

// ---- wabbajack --------------------------------------------------------------
struct WjGalleryRow {
    std::pmr::string title;
    std::pmr::string machine_url;
    std::pmr::string repository;
    std::pmr::string author;
    std::pmr::string version;
    std::pmr::string description;
    std::int64_t download_size = 0;
    std::int64_t archives_size = 0;
    std::int64_t installed_size = 0;
    std::int64_t archive_count = 0;
    bool nsfw = false;
    bool unavailable = false;
};
struct WjSearchData {
    std::pmr::string game;
    std::pmr::string query;
    std::int64_t total = 0;
    std::pmr::vector<WjGalleryRow> lists;
};
struct WjCount {
    std::pmr::string name;
    std::int64_t count = 0;
    std::int64_t size = 0;
    bool supported = true;
};
struct WjInspectData {
    std::pmr::string name;
    std::pmr::string author;
    std::pmr::string version;
    std::pmr::string description;
    std::pmr::string game_type;
    std::pmr::string game_id;
    bool nsfw = false;
    bool game_matches = true;   // 清单的游戏与本实例的游戏一致
    std::pmr::string file;      // 本地 .wabbajack 路径
    std::int64_t archive_count = 0;
    std::int64_t archive_size = 0;
    std::int64_t directive_count = 0;
    std::int64_t supported_directives = 0;
    std::pmr::vector<WjCount> sources;     // 按下载来源
    std::pmr::vector<WjCount> directives;  // 按指令类型
    std::pmr::string verdict;  // full | partial | none：我们能装多少
};
struct WjInstallData {
    std::pmr::string status;  // complete|incomplete
    std::pmr::string instance;
    std::int64_t archives_total = 0;
    std::int64_t archives_done = 0;
    std::int64_t files_written = 0;
    std::int64_t files_failed = 0;
    std::pmr::vector<CollectionPendingRow> pending;  // key=压缩包/指令类型，kind 同上
    std::pmr::vector<std::pmr::string> failures;
    std::pmr::vector<std::pmr::string> notes;
};

struct OutdatedRow {
    std::pmr::string name;
    std::int64_t nexus_id = 0;
    std::pmr::string installed_version;
    std::pmr::string latest_version;
    bool outdated = false;  // 版本字符串不同（不是语义化比较）
    std::pmr::string updated_at;
};
struct OutdatedData {
    std::int64_t checked = 0;
    std::int64_t outdated_count = 0;
    std::pmr::vector<OutdatedRow> mods;
};

// ---- nxm / default instance ---------------------------------------------------
struct NxmMatchRow {
    std::pmr::string collection;
    std::pmr::string key;
    std::pmr::string name;
};
struct NxmHandleData {
    std::pmr::string instance;
    std::pmr::string path;
    std::uint64_t size = 0;
    std::pmr::string game;
    std::int64_t mod_id = 0;
    std::int64_t file_id = 0;
    std::pmr::vector<NxmMatchRow> matches;  // 正在等这个文件的集合 mod（已记下压缩包，下次 `collection install` 会继续）
};
struct NxmRegisterData {
    std::pmr::string desktop_file;
    std::pmr::string exec;
    bool mime_registered = false;  // xdg-mime 是否成功
};
struct DefaultInstanceData {
    std::pmr::string path;  // 空 = 未设置
    bool changed = false;
};

struct ExecutableRow {
    std::pmr::string title;
    std::pmr::string binary;
    std::pmr::string arguments;
    std::pmr::string working_dir;
    std::pmr::string farm_path;  // 非空 = 在农场里相对这个路径运行；空 = 直接用 binary 的绝对路径
    bool hide = false;
};
struct ExecutablesData {
    std::pmr::vector<ExecutableRow> executables;
};

// ---- run --------------------------------------------------------------------
struct RunData {
    std::pmr::string exe;
    std::pmr::string title;  // 用 --title 启动时的登记名
    bool dry_run = false;
    bool detached = false;
    bool synced_plugins = false;
    int game_exit_code = 0;
    std::size_t captured = 0;  // 移回 overwrite 的文件数（启动前残留 + 退出后）
    std::pmr::vector<std::pmr::string> argv;
    std::pmr::string cwd;
};

// ---- plan / status / apply / unlink ---------------------------------------
struct OpCounts {
    std::size_t mkdir = 0;
    std::size_t link = 0;
    std::size_t relink = 0;
    std::size_t remove = 0;
    std::size_t rmdir = 0;
};

struct OpRow {
    std::pmr::string kind;   // mkdir|link|relink|remove|rmdir
    std::pmr::string path;   // 相对农场根
    std::pmr::string target;  // link/relink 的目标（其余为空）
};

struct PlanData {
    std::pmr::vector<OpRow> ops;
    std::size_t count = 0;
    OpCounts counts;
    std::size_t warnings = 0;  // merge 警告条数
};

struct StatusData {
    bool in_sync = false;
    std::size_t pending = 0;
    std::pmr::string farm_path;
    bool farm_exists = false;
};

struct ApplyData {
    std::size_t applied = 0;
    bool changed = false;
    std::pmr::string farm_path;
};

struct UnlinkData {
    bool removed = false;
    std::pmr::string farm_path;
};

}  // namespace cli
