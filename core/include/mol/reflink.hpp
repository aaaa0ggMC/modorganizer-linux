#pragma once
// 解压后文件的复用（btrfs / xfs 的 reflink）：
//   reflink_tree  整个目录「复制」成共享数据块的独立副本——瞬间完成、不占额外空间，之后各改各的（文件系统层 COW）
//   dedupe_tree   新装的 mod 与参考 mod 里内容相同的文件改成共享数据块（FIDEDUPERANGE：内核逐字节比对，不同就不动）
// 都只在同一个支持 reflink 的文件系统上起作用；不支持时 reflink_tree 返回 false（调用方退回正常安装），dedupe_tree 什么都不做。
#include <cstdint>
#include <filesystem>

namespace mol {

// dst 必须不存在。成功复制全部文件返回 true；任何一个文件不能 reflink（跨文件系统、ext4……）→ 删掉 dst 已建的部分，返回 false。
bool reflink_tree(const std::filesystem::path& src, const std::filesystem::path& dst);

struct DedupeStats {
    std::uint64_t files = 0;  // 改成共享的文件数
    std::uint64_t bytes = 0;  // 共享的字节数（省下的空间）
};
// 对 dir 里每个文件，在 ref 里找同相对路径（大小写不敏感）且同大小的文件，请内核去重。
DedupeStats dedupe_tree(const std::filesystem::path& dir, const std::filesystem::path& ref);

}  // namespace mol
