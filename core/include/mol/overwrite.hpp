#pragma once
// overwrite 捕获：游戏在农场里新建的真实文件（不是符号链接）移回 <overwrite>/（与 MO2 的 overwrite 语义一致）。
// 农场 Data/ 下的文件 → <overwrite>/<相对 Data 的路径>；Data/ 之外的 → <实例根>/overwrite-root/<相对农场根的路径>
// （不参与合并，仅避免它挡住下一次 apply）。已存在同名目标时覆盖（游戏的较新版本为准）。
#include <cstddef>
#include <span>
#include <string>

#include "mol/instance.hpp"

namespace mol {

// 目标已存在同名文件时，旧文件先移到 <实例根>/overwrite-backup/<相对路径>（再旧的备份被替换），不会静默丢失。
// 调用方应先 require_farm_idle。
// 返回移动的文件数。农场不存在 → 0。io 失败 → Error{io_error}。
// 工具运行期间 libmol-cow 做的写时复制在 <实例>/.mol-cow.log 里有记录：内容与原文件相同的副本（只是以写方式
// 打开、没改）直接丢弃，下次 apply 恢复成链接；不在 manifest 里的符号链接（工具挪动/改名了我们的链接）先换成
// 真实副本再收回。
std::size_t capture_overwrite(const Instance& inst);

// libmol-cow 的日志位置（runner 通过 MOL_COW_LOG 交给注入库）。
std::string cow_log_path(const Instance& inst);
// 本轮（上次 capture 之后）写时复制的次数，其中走 reflink 的次数（btrfs/xfs 上应当等于 copies）。capture 前调用。
struct CowStats {
    std::size_t copies = 0;
    std::size_t reflinked = 0;
};
CowStats cow_stats(const Instance& inst);
// src 的内容放到 dst：先试 reflink（btrfs/xfs 上瞬间完成、不占额外空间），不行就普通复制。成功返回 true。
bool clone_or_copy_file(const std::string& src, const std::string& dst);

// 「永久驻留」：把 <overwrite>/ 里匹配 filters 的文件**移进真实的游戏 Data 目录**（Steam 直接启动游戏也能看到，
// 例如 Creations）。这是唯一会向游戏目录写入的操作，所以默认只预览（execute=false）。
// filters：fnmatch 通配（大小写不敏感），匹配相对 overwrite 的路径；空 → 不匹配任何文件（调用方须显式给）。
// 目标已存在 → 跳过并记入 skipped（绝不覆盖游戏文件）。目标目录大小写按游戏目录里已有名字解析。
// 执行时调用方应先 require_farm_idle，之后 apply 农场。
struct PromoteEntry {
    using allocator_type = mol::allocator_type;
    string path;  // 相对 overwrite
    string dest;  // 将要/已经落到的真实路径
    bool skipped = false;  // 目标已存在
    explicit PromoteEntry(allocator_type a = {}) : path(a), dest(a) {}
    PromoteEntry(const PromoteEntry& o, allocator_type a) : path(o.path, a), dest(o.dest, a), skipped(o.skipped) {}
    PromoteEntry(PromoteEntry&& o, allocator_type a) : path(std::move(o.path), a), dest(std::move(o.dest), a), skipped(o.skipped) {}
    PromoteEntry(const PromoteEntry&) = default;
    PromoteEntry(PromoteEntry&&) = default;
    PromoteEntry& operator=(const PromoteEntry&) = default;
    PromoteEntry& operator=(PromoteEntry&&) = default;
};
vector<PromoteEntry> promote_overwrite(const Instance& inst, std::span<const std::string> filters, bool execute,
                                       mr* mem = default_mr());

// 是否有进程在使用农场（命令行含农场路径——正斜杠或 Wine 的反斜杠形式——或 cwd 在农场内）。
bool farm_in_use(const Instance& inst);
// 占用农场的第一个进程："pid N: 命令行"；没有返回空。
std::string farm_user(const Instance& inst);
// farm_in_use → 抛 Error{farm_busy}。
void require_farm_idle(const Instance& inst);

}  // namespace mol
