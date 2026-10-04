#pragma once
// plugins sync：把游戏层 mappings()（profile 的 plugins.txt/loadorder.txt → 游戏 AppData）
// 物化为符号链接，使游戏写入 plugins.txt 时写穿到 profile 文件（等价于 MO2 用 usvfs 做的虚拟化）。
// 幂等；目标位置上已有的真实文件不会被删除，而是改名为 "<名>.mol-backup"。
#include "mol/error.hpp"
#include "mol/game_host.hpp"
#include "mol/instance.hpp"
#include "mol/pmr.hpp"

namespace mol {

struct SyncEntry {
    using allocator_type = mol::allocator_type;
    string source;       // profile 里的真实文件
    string destination;  // 游戏期望的位置（将成为指向 source 的符号链接）
    string action;       // "ok"（已是期望状态）| "link" | "relink" | "backup+link" | "skip-missing-source"
    explicit SyncEntry(allocator_type a = {}) : source(a), destination(a), action(a) {}
    SyncEntry(const SyncEntry& o, allocator_type a) : source(o.source, a), destination(o.destination, a), action(o.action, a) {}
    SyncEntry(SyncEntry&& o, allocator_type a) : source(std::move(o.source), a), destination(std::move(o.destination), a), action(std::move(o.action), a) {}
    SyncEntry(const SyncEntry&) = default;
    SyncEntry(SyncEntry&&) = default;
    SyncEntry& operator=(const SyncEntry&) = default;
    SyncEntry& operator=(SyncEntry&&) = default;
};

struct SyncReport {
    using allocator_type = mol::allocator_type;
    string profile;
    bool initialized_profile = false;  // 因 profile 里没有 plugins.txt 而调用了 initializeProfile
    bool changed = false;
    vector<SyncEntry> entries;
    explicit SyncReport(allocator_type a = {}) : profile(a), entries(a) {}
    SyncReport(const SyncReport& o, allocator_type a) : profile(o.profile, a), initialized_profile(o.initialized_profile), changed(o.changed), entries(o.entries, a) {}
    SyncReport(SyncReport&& o, allocator_type a) : profile(std::move(o.profile), a), initialized_profile(o.initialized_profile), changed(o.changed), entries(std::move(o.entries), a) {}
    SyncReport(const SyncReport&) = default;
    SyncReport(SyncReport&&) = default;
    SyncReport& operator=(const SyncReport&) = default;
    SyncReport& operator=(SyncReport&&) = default;
};

// 失败 → Error{profile_not_found | game_unavailable | io_error}。
SyncReport sync_plugins(const Instance& inst, const Game& game, mr* mem = default_mr());

}  // namespace mol
