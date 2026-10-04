#pragma once
// 把压缩包（7z/zip/rar…）安装为 mods/<name>/：解压 → 去掉多余的顶层目录 → 判定布局 → 写 modlist（最高优先级、启用）。
// 解压调用外部 `7z`（或 `bsdtar`）；没有则 Error{io_error}。不使用 shell；拒绝含符号链接或越界路径的压缩包。
// 布局判定：
//   * 顶层有 .exe/.dll 文件 → 「根目录型」（目录结构镜像游戏根，如 SKSE64）：保持原样，meta.ini 写 mol_root=true；
//   * 否则若顶层只有一个 Data 目录 → 取其内容作为 mod 根（普通 mod）；
//   * 否则原样（普通 mod）。
// 不处理 FOMOD 安装器。
#include <string_view>

#include <functional>
#include <optional>
#include <vector>

#include "mol/fomod.hpp"
#include "mol/instance.hpp"

namespace mol {

// 压缩包带 FOMOD（fomod/ModuleConfig.xml）时的处理方式。
//   Unset    → 报错 Error{fomod_choices_required}（调用方必须明确选择，避免静默装出不是用户想要的东西）
//   Raw      → 忽略 FOMOD，按普通压缩包原样安装
//   Defaults → 全部用默认选择
//   Choices  → 用 InstallOptions::choices（缺失的可见组报错；use_defaults_for_missing 为真时缺失的组用默认）
enum class FomodMode { Unset, Raw, Defaults, Choices };

struct InstallOptions {
    std::string_view name;
    bool force_root = false;
    std::string_view profile;
    FomodMode fomod = FomodMode::Unset;
    fomod::Choices choices;
    bool use_defaults_for_missing = false;
    fomod::Env fomod_env;  // file_state 为空时安装会自己补上（查游戏 Data 与已启用 mod）
};

struct InstallResult {
    using allocator_type = mol::allocator_type;
    string name;
    string path;
    bool root = false;
    std::size_t files = 0;
    bool fomod = false;                  // 经过了 FOMOD 安装
    std::vector<std::string> missing;    // FOMOD 引用了、但压缩包里找不到的源
    explicit InstallResult(allocator_type a = {}) : name(a), path(a) {}
    InstallResult(const InstallResult& o, allocator_type a) : name(o.name, a), path(o.path, a), root(o.root), files(o.files), fomod(o.fomod), missing(o.missing) {}
    InstallResult(InstallResult&& o, allocator_type a) : name(std::move(o.name), a), path(std::move(o.path), a), root(o.root), files(o.files), fomod(o.fomod), missing(std::move(o.missing)) {}
    InstallResult(const InstallResult&) = default;
    InstallResult(InstallResult&&) = default;
    InstallResult& operator=(const InstallResult&) = default;
    InstallResult& operator=(InstallResult&&) = default;
};

// name 空 → 压缩包文件名（去扩展名）。mods/<name> 已存在 → Error{invalid_argument}（不覆盖）。
// force_root: 强制按根目录型处理。失败时清理半成品目录。
InstallResult install_archive(const Instance& inst, std::string_view archive, const InstallOptions& opt, mr* mem = default_mr());
// 兼容旧调用：FOMOD 一律按 Raw 处理。
InstallResult install_archive(const Instance& inst, std::string_view archive, std::string_view name = {},
                              bool force_root = false, std::string_view profile = {}, mr* mem = default_mr());

// FOMOD 的文件依赖求值：游戏 Data 或任一已启用 mod 里有该文件 → "Active"，否则 "Missing"。
std::function<std::string(std::string_view)> fomod_file_state(const Instance& inst, std::string_view profile = {}, mr* mem = default_mr());

// 只读：压缩包里有没有 FOMOD；有则把 fomod/ 下的文件（含 ModuleConfig.xml 引用的图片不在内）解到临时目录并解析。
// 临时目录在 inst.downloads_dir 下，函数返回前清理。没有 FOMOD → nullopt。
std::optional<fomod::Config> read_archive_fomod(const Instance& inst, std::string_view archive);

}  // namespace mol
