#pragma once
// plugins sync：把游戏层 mappings()（profile 的 plugins.txt/loadorder.txt → 游戏 AppData）
// 物化为符号链接，使游戏写入 plugins.txt 时写穿到 profile 文件（等价于 MO2 用 usvfs 做的虚拟化）。
// 幂等；目标位置上已有的真实文件不会被删除，而是改名为 "<名>.mol-backup"。
#include "mol/error.hpp"
#include "mol/game_host.hpp"
#include "mol/instance.hpp"
#include "mol/pmr.hpp"

#include <filesystem>

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
// 除了上游 mappings() 里的 plugins.txt/loadorder.txt，还会按 MO2 的 profile 设置链接本地 ini 与存档目录：
//   * profile 的 settings.ini 里 [General] LocalSettings=true → profile 目录里的 Skyrim.ini / SkyrimPrefs.ini / SkyrimCustom.ini（大小写不敏感）
//     链接到前缀的 Documents/My Games/Skyrim Special Edition/（游戏写它们时写穿回 profile，与 MO2 的 usvfs 语义一致；
//     目标处已有的真实 ini 先改名 .mol-backup）；
//   * LocalSaves=true 且 profile 有 saves/ 目录 → 把前缀里的 Saves 链接过去。目标处已有**非空的真实目录**时不碰（只给 skipped 说明）。
SyncReport sync_plugins(const Instance& inst, const Game& game, mr* mem = default_mr());
// 只做 profile 本地 ini / 存档那部分（不需要游戏层；sync_plugins 内部也调用它）。结果追加到 rep。
void sync_profile_settings(const Instance& inst, SyncReport& rep, mr* mem = default_mr());
// dst 同目录下只差大小写的同名条目（Wine 打开时优先大小写完全一致的那个，会遮住我们的链接）：
// 真实文件改名为 <原名>.mol-backup（已有备份 → Error{io_error}），符号链接直接删。返回处理的个数。
std::size_t backup_case_variants(const std::filesystem::path& dst);

}  // namespace mol
