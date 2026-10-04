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
#include <string>
#include <vector>

#include "mol/pmr.hpp"

namespace cli {

// envelope 的 warnings/errors 条目
struct Err {
    std::pmr::string code;
    std::pmr::string message;
    std::pmr::string path;  // 可空
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

// ---- run --------------------------------------------------------------------
struct RunData {
    std::pmr::string exe;
    bool dry_run = false;
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
