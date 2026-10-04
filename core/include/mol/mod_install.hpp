#pragma once
// 把压缩包（7z/zip/rar…）安装为 mods/<name>/：解压 → 去掉多余的顶层目录 → 判定布局 → 写 modlist（最高优先级、启用）。
// 解压调用外部 `7z`（或 `bsdtar`）；没有则 Error{io_error}。不使用 shell；拒绝含符号链接或越界路径的压缩包。
// 布局判定：
//   * 顶层有 .exe/.dll 文件 → 「根目录型」（目录结构镜像游戏根，如 SKSE64）：保持原样，meta.ini 写 mol_root=true；
//   * 否则若顶层只有一个 Data 目录 → 取其内容作为 mod 根（普通 mod）；
//   * 否则原样（普通 mod）。
// 不处理 FOMOD 安装器。
#include <string_view>

#include "mol/instance.hpp"

namespace mol {

struct InstallResult {
    using allocator_type = mol::allocator_type;
    string name;
    string path;
    bool root = false;
    std::size_t files = 0;
    explicit InstallResult(allocator_type a = {}) : name(a), path(a) {}
    InstallResult(const InstallResult& o, allocator_type a) : name(o.name, a), path(o.path, a), root(o.root), files(o.files) {}
    InstallResult(InstallResult&& o, allocator_type a) : name(std::move(o.name), a), path(std::move(o.path), a), root(o.root), files(o.files) {}
    InstallResult(const InstallResult&) = default;
    InstallResult(InstallResult&&) = default;
    InstallResult& operator=(const InstallResult&) = default;
    InstallResult& operator=(InstallResult&&) = default;
};

// name 空 → 压缩包文件名（去扩展名）。mods/<name> 已存在 → Error{invalid_argument}（不覆盖）。
// force_root: 强制按根目录型处理。失败时清理半成品目录。
InstallResult install_archive(const Instance& inst, std::string_view archive, std::string_view name = {},
                              bool force_root = false, std::string_view profile = {}, mr* mem = default_mr());

}  // namespace mol
