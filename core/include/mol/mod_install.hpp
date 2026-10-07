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
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "mol/fomod.hpp"
#include "mol/instance.hpp"

namespace mol {

// 压缩包带 FOMOD（fomod/ModuleConfig.xml）时的处理方式。
//   Unset    → 报错 Error{fomod_choices_required}（调用方必须明确选择，避免静默装出不是用户想要的东西）
//   Raw      → 忽略 FOMOD，按普通压缩包原样安装
//   Defaults → 全部用默认选择
//   Choices  → 用 InstallOptions::choices（缺失的可见组报错；use_defaults_for_missing 为真时缺失的组用默认）
//   Replicate→ 不跑 FOMOD：按 InstallOptions::replicate 的 (路径, md5) 从压缩包里挑文件（集合清单的 `hashes`）；
//              压缩包没有 FOMOD 时同样生效。找不到的记进 InstallResult::missing；一个都找不到 → Error{invalid_argument}。
enum class FomodMode { Unset, Raw, Defaults, Choices, Replicate };

struct InstallOptions {
    std::string_view name;
    bool force_root = false;
    std::string_view profile;
    FomodMode fomod = FomodMode::Unset;
    fomod::Choices choices;
    bool use_defaults_for_missing = false;
    bool fomod_lenient = false;  // Choices 来自集合清单：对不上的组退回默认、不存在的插件忽略，记进 InstallResult::fomod_notes
    fomod::Env fomod_env;  // file_state 为空时安装会自己补上（查游戏 Data 与已启用 mod）
    std::vector<std::pair<std::string, std::string>> replicate;  // FomodMode::Replicate：(相对 mod 根的路径, md5)
    bool replace_existing = false;  // mods/<name> 已存在时原地替换（新内容装好后才换掉旧目录；modlist 不重复添加）
};

struct InstallResult {
    using allocator_type = mol::allocator_type;
    string name;
    string path;
    bool root = false;
    std::size_t files = 0;
    bool fomod = false;                  // 经过了 FOMOD 安装
    std::vector<std::string> missing;    // FOMOD 引用了、但压缩包里找不到的源
    std::vector<std::string> fomod_notes;  // fomod_lenient 时的退让说明
    explicit InstallResult(allocator_type a = {}) : name(a), path(a) {}
    InstallResult(const InstallResult& o, allocator_type a)
        : name(o.name, a), path(o.path, a), root(o.root), files(o.files), fomod(o.fomod), missing(o.missing), fomod_notes(o.fomod_notes) {}
    InstallResult(InstallResult&& o, allocator_type a)
        : name(std::move(o.name), a), path(std::move(o.path), a), root(o.root), files(o.files), fomod(o.fomod), missing(std::move(o.missing)),
          fomod_notes(std::move(o.fomod_notes)) {}
    InstallResult(const InstallResult&) = default;
    InstallResult(InstallResult&&) = default;
    InstallResult& operator=(const InstallResult&) = default;
    InstallResult& operator=(InstallResult&&) = default;
};

// name 空 → 压缩包文件名（去扩展名）。mods/<name> 已存在 → Error{invalid_argument}（不覆盖；replace_existing 时替换）。
// force_root: 强制按根目录型处理。失败时清理半成品目录。
InstallResult install_archive(const Instance& inst, std::string_view archive, const InstallOptions& opt, mr* mem = default_mr());
// 兼容旧调用：FOMOD 一律按 Raw 处理。
InstallResult install_archive(const Instance& inst, std::string_view archive, std::string_view name = {},
                              bool force_root = false, std::string_view profile = {}, mr* mem = default_mr());

// 这一层是不是游戏 Data 根（顶层有 meshes/、scripts/、*.esp …；规则同上游 SkyrimSEModDataChecker）。
bool is_data_root_dir(std::string_view dir);

// install_archive 用的 mod 目录名清理规则（去掉路径/Windows 非法字符、首尾的点与空格）。
std::string sanitize_mod_name(std::string_view name);
// mods/ 下或 modlist 里是否已有该名字（按 sanitize_mod_name 后、大小写不敏感比较）。
bool mod_name_taken(const Instance& inst, std::string_view name, std::string_view profile = {});

// 把任意压缩包解到 dest（须存在）；用外部 7z/7zz/bsdtar；不做安全校验（调用方自行 validate）。失败 → Error{io_error}。
void extract_archive(std::string_view archive, std::string_view dest);

// 用 names（低→高优先级）重排 profile 的 modlist：先从列表里删掉这些名字，再按给定顺序追加到最高优先级处。
// 不在 modlist 里的名字会被新增（启用）。其余条目保持相对顺序。
void reorder_mods(const Instance& inst, std::span<const string> names_low_to_high, std::string_view profile = {});

// FOMOD 的文件依赖求值：游戏 Data 或任一已启用 mod 里有该文件 → "Active"，否则 "Missing"。
std::function<std::string(std::string_view)> fomod_file_state(const Instance& inst, std::string_view profile = {}, mr* mem = default_mr());

// 只读：压缩包里有没有 FOMOD；有则把 fomod/ 下的文件（含 ModuleConfig.xml 引用的图片不在内）解到临时目录并解析。
// 临时目录在 inst.downloads_dir 下，函数返回前清理。没有 FOMOD → nullopt。
std::optional<fomod::Config> read_archive_fomod(const Instance& inst, std::string_view archive);
// 把 FOMOD 引用的图片（配置里的路径，如 "images\\header.png"；相对模块根，大小写不敏感）解到 dest_dir（会创建），
// 只解这些文件，不解整个压缩包。返回 配置里的路径 → 解出的绝对路径；找不到的图片不在结果里。
// 需要 7z/7zz（bsdtar 没有大小写不敏感的过滤）；都没有时返回空。
std::map<std::string, std::string> extract_fomod_images(const Instance& inst, std::string_view archive, const std::vector<std::string>& images,
                                                        std::string_view dest_dir);

}  // namespace mol
