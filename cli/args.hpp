#pragma once
// mo-linux CLI 参数的数据结构、选项规格表与路径工具（纯逻辑，无 alib6 / 无 import）。
//
// 真正的命令行解析由 alib6 `Command` 完成（路由、`--name=value`、`--name value`、
// 前置/后置选项、`--` 终止符）。本文件提供：
//   * 选项规格表（`main` 注册与 `cli/parse` 校验的**唯一来源**，避免两处漂移）；
//   * `GlobalOptions` / `ParsedArgs` 数据结构与访问器；
//   * 路径工具（`abs_path` / `resolve_instance_dir` / `path_exists`）。
// `CommandInput` → 内部类型的适配器在 `cli/parse.{hpp,cpp}`（依赖 alib6）。
//
// 注意 alib6 的 `token_keys` 规则（见 modules/alib6/core/cmd.cpp）：long_name/short_name
// 任一非空时，规范名 `name` 不参与 token 匹配——所以 `name="instance"` 可以安全地与
// 路由 token `instance` 并存（这正是 alib6 修复前后的行为差异）。
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mol/pmr.hpp"

namespace cli {

// ---------------------------------------------------------------------------
// 选项规格表
// ---------------------------------------------------------------------------
struct OptionSpec {
    std::string_view name;         // 规范名（alib6 get/has 的查找键）
    std::string_view short_name;   // 如 "-i"，可空
    std::string_view long_name;    // 如 "--instance"，可空
    std::string_view description;  // help 文本
    bool takes_value = true;       // false = 开关（Toggle）
};

// 全局选项：每个子命令都接受
constexpr OptionSpec kOptGlobal[] = {
    {"instance", "-i", "--instance", "Instance directory (default: $MOL_INSTANCE or cwd)", true},
    {"profile", "-p", "--profile", "Override the profile from the instance config", true},
    {"events", "", "--events", "Progress events (NDJSON): fd:N | fifo:PATH | unix:PATH", true},
};

// 全局开关（Toggle）
constexpr OptionSpec kTogGlobal[] = {
    {"json", "-j", "--json", "Emit a machine-readable JSON envelope on stdout", false},
    {"quiet", "-q", "--quiet", "Do not log to stderr", false},
    {"help", "-h", "--help", "Show this help and exit", false},
};

// 各子命令的专有选项（没有专有选项的命令用空 span：{}）
constexpr OptionSpec kOptInstanceInit[] = {
    {"game-dir", "", "--game-dir", "Game directory (must exist)", true},
    {"prefix", "", "--prefix", "Wine/Proton prefix directory", true},
    {"prefix-user", "", "--prefix-user", "User name inside the prefix", true},
    {"runner", "", "--runner", "Runner kind: proton|wine", true},
    {"proton-path", "", "--proton-path", "Proton directory (contains the proton script)", true},
    {"steam-root", "", "--steam-root", "STEAM_COMPAT_CLIENT_INSTALL_PATH", true},
};
constexpr OptionSpec kOptModsMove[] = {
    {"to", "", "--to", "Target priority index (0 = lowest)", true},
};
constexpr OptionSpec kOptRun[] = {
    {"exe", "", "--exe", "Executable relative to the farm (default SkyrimSE.exe)", true},
    {"skse", "", "--skse", "Run skse64_loader.exe", false},
    {"detach", "", "--detach", "Return right after launching (do not wait; skips overwrite capture)", false},
    {"dry-run", "", "--dry-run", "Print the launch command only; change nothing", false},
};
constexpr OptionSpec kOptPromote[] = {
    {"filter", "", "--filter", "Glob(s) relative to overwrite/, comma separated, case-insensitive (required)", true},
    {"yes", "", "--yes", "Actually move files into the real game Data directory (default: preview only)", false},
};
constexpr OptionSpec kOptNexusLogin[] = {
    {"key-file", "", "--key-file", "Read the API key from this file (default: stdin)", true},
};
constexpr OptionSpec kOptNexusFiles[] = {
    {"mod", "", "--mod", "Nexus mod id", true},
};
constexpr OptionSpec kOptNexusDownload[] = {
    {"nxm", "", "--nxm", "nxm:// link (needed for free accounts)", true},
    {"mod", "", "--mod", "Nexus mod id", true},
    {"file", "", "--file", "Nexus file id", true},
};
constexpr OptionSpec kOptModsInstall[] = {
    {"name", "", "--name", "Mod name (default: archive file name)", true},
    {"root", "", "--root", "Force root-style layout (mirrors the game directory)", false},
};
constexpr OptionSpec kOptConflicts[] = {
    {"mod", "", "--mod", "Only conflicts involving this mod", true},
};

// ---------------------------------------------------------------------------
// 全局选项
// ---------------------------------------------------------------------------
struct GlobalOptions {
    using allocator_type = mol::allocator_type;

    mol::string instance;  // -i / --instance（原样；解析/绝对化见 resolve_instance_dir）
    bool has_instance = false;
    mol::string profile;  // -p / --profile
    bool has_profile = false;
    bool json = false;   // -j / --json
    bool quiet = false;  // -q / --quiet
    mol::string events;  // --events TARGET
    bool has_events = false;

    GlobalOptions() = default;
    explicit GlobalOptions(allocator_type a)
        : instance(a), profile(a), events(a) {}
    GlobalOptions(const GlobalOptions& o, allocator_type a)
        : instance(o.instance, a), has_instance(o.has_instance), profile(o.profile, a),
          has_profile(o.has_profile), json(o.json), quiet(o.quiet), events(o.events, a),
          has_events(o.has_events) {}
    GlobalOptions(GlobalOptions&& o, allocator_type a)
        : instance(std::move(o.instance), a), has_instance(o.has_instance),
          profile(std::move(o.profile), a), has_profile(o.has_profile), json(o.json),
          quiet(o.quiet), events(std::move(o.events), a), has_events(o.has_events) {}
    GlobalOptions(const GlobalOptions&) = default;
    GlobalOptions(GlobalOptions&&) = default;
    GlobalOptions& operator=(const GlobalOptions&) = default;
    GlobalOptions& operator=(GlobalOptions&&) = default;
};

// 实例目录：-i > $MOL_INSTANCE > 当前目录；总是绝对化后的 Unix 路径。
mol::string resolve_instance_dir(const GlobalOptions& g, mol::mr* mem);
// 把任意路径绝对化（不要求已存在）。
mol::string abs_path(std::string_view p, mol::mr* mem);
bool path_exists(std::string_view p);

// ---------------------------------------------------------------------------
// 子命令参数
// ---------------------------------------------------------------------------
struct ParsedArgs {
    using allocator_type = mol::allocator_type;

    mol::vector<mol::string> positionals;
    mol::vector<std::pair<mol::string, mol::string>> options;  // 规范 long 名 → 值（开关为空）
    mol::string error;                                         // 非空 = 用法错误

    ParsedArgs() = default;
    explicit ParsedArgs(allocator_type a)
        : positionals(a), options(a), error(a) {}
    ParsedArgs(const ParsedArgs& o, allocator_type a)
        : positionals(o.positionals, a), options(o.options, a), error(o.error, a) {}
    ParsedArgs(ParsedArgs&& o, allocator_type a)
        : positionals(std::move(o.positionals), a), options(std::move(o.options), a),
          error(std::move(o.error), a) {}
    ParsedArgs(const ParsedArgs&) = default;
    ParsedArgs(ParsedArgs&&) = default;
    ParsedArgs& operator=(const ParsedArgs&) = default;
    ParsedArgs& operator=(ParsedArgs&&) = default;

    [[nodiscard]] bool ok() const { return error.empty(); }
    [[nodiscard]] bool has(std::string_view name) const;
    [[nodiscard]] mol::string get(std::string_view name, std::string_view def, mol::mr* mem) const;
    [[nodiscard]] bool get_bool(std::string_view name, bool def) const;
};

}  // namespace cli
