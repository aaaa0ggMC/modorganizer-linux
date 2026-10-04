#pragma once
// SKSE64 一键安装：按游戏版本推出需要的运行时 dll → 在 Nexus（Skyrim SE 的 SKSE64，mod 30379）选主文件 →
// 下载（已下载则复用）→ 安装为根目录型 mod「SKSE64」→ 校验运行时 dll 确实与游戏版本匹配。幂等。
#include <functional>
#include <string_view>

#include "mol/instance.hpp"
#include "mol/nexus.hpp"

namespace mol {

constexpr std::int64_t kSkseNexusModId = 30379;

struct SkseResult {
    using allocator_type = mol::allocator_type;
    string game_version;
    string runtime_dll;
    bool installed = false;   // 本次是否新装了
    string mod_name;          // 提供该 dll 的 mod；已存在于游戏目录时为空
    string file_name;         // 使用的 Nexus 文件
    std::int64_t file_id = 0;
    bool downloaded = false;  // false = 复用了已下载的文件或无需下载
    explicit SkseResult(allocator_type a = {}) : game_version(a), runtime_dll(a), mod_name(a), file_name(a) {}
    SkseResult(const SkseResult& o, allocator_type a) : game_version(o.game_version, a), runtime_dll(o.runtime_dll, a), installed(o.installed), mod_name(o.mod_name, a), file_name(o.file_name, a), file_id(o.file_id), downloaded(o.downloaded) {}
    SkseResult(SkseResult&& o, allocator_type a) : game_version(std::move(o.game_version), a), runtime_dll(std::move(o.runtime_dll), a), installed(o.installed), mod_name(std::move(o.mod_name), a), file_name(std::move(o.file_name), a), file_id(o.file_id), downloaded(o.downloaded) {}
    SkseResult(const SkseResult&) = default;
    SkseResult(SkseResult&&) = default;
    SkseResult& operator=(const SkseResult&) = default;
    SkseResult& operator=(SkseResult&&) = default;
};

// 错误码：invalid_argument（无法由游戏版本推出 dll 名）、mod_not_found（Nexus 上没有合适的主文件）、
// skse_mismatch（装完后仍没有匹配的 dll：该 SKSE 版本尚不支持这个游戏版本）、以及 Nexus/IO 的各种码。
SkseResult install_skse(const Instance& inst, std::string_view game_version, const NexusClient& client,
                        const std::function<bool(std::uint64_t, std::uint64_t)>& progress = {}, mr* mem = default_mr());

}  // namespace mol
