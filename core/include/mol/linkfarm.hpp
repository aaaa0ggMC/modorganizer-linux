#pragma once
// 把 MergeResult 物化为"链接农场"目录，幂等、可 diff、可清理。
#include <cstddef>
#include <functional>
#include <string_view>

#include "mol/merge.hpp"
#include "mol/pmr.hpp"

namespace mol {

enum class OpKind {
    Mkdir,   // 创建真实目录
    Link,    // 新建符号链接 path -> target
    Relink,  // 链接已存在但目标不对：删除后重建
    Remove,  // 删除我们创建过、但已不在期望树中的文件/链接
    Rmdir,   // 删除我们创建过、已不需要且为空的目录
};

struct Op {
    using allocator_type = mol::allocator_type;
    OpKind kind = OpKind::Link;
    string path;    // 相对农场根，规范大小写
    string target;  // Link/Relink 的目标（绝对路径）

    explicit Op(allocator_type a = {}) : path(a), target(a) {}
    Op(const Op& o, allocator_type a) : kind(o.kind), path(o.path, a), target(o.target, a) {}
    Op(Op&& o, allocator_type a) : kind(o.kind), path(std::move(o.path), a), target(std::move(o.target), a) {}
    Op(const Op&) = default;
    Op(Op&&) = default;
    Op& operator=(const Op&) = default;
    Op& operator=(Op&&) = default;
};

struct Plan {
    using allocator_type = mol::allocator_type;
    vector<Op> ops;  // 顺序即执行顺序（先清理旧的，再 Mkdir/Link；父先于子）
    explicit Plan(allocator_type a = {}) : ops(a) {}
    Plan(const Plan& o, allocator_type a) : ops(o.ops, a) {}
    Plan(Plan&& o, allocator_type a) : ops(std::move(o.ops), a) {}
    Plan(const Plan&) = default;
    Plan(Plan&&) = default;
    Plan& operator=(const Plan&) = default;
    Plan& operator=(Plan&&) = default;
    bool empty() const { return ops.empty(); }
};

constexpr const char* kFarmMarker = ".mol-farm.json";  // 农场根下的标记 + manifest

// 只读：对比 expected 与 root 的实际内容 + manifest，算出需要做的操作。
// root 必须不存在、为空，或含 kFarmMarker；否则抛 std::runtime_error（拒绝碰用户目录）。
Plan plan_farm(const MergeResult& expected, std::string_view root, mr* mem = default_mr());

// 执行 plan，并更新 manifest（manifest 记录我们创建的所有相对路径）。
// 幂等：对同一 expected 连续 plan 两次，第二次必须 empty()。
// progress（可空）：每完成一批 op 回调 (done, total)，最后一次 done == total。
// manifest 在执行中按时间做检查点（约每秒一次）并在结束/失败时落盘，进程中途被杀最多丢最近一秒的登记。
using ApplyProgress = std::function<void(std::size_t done, std::size_t total)>;
void apply_farm(const Plan& plan, std::string_view root, const ApplyProgress& progress = {});

// manifest 里登记的、由我们创建的全部相对路径（字节序）。root 不是农场 → 空。
vector<string> farm_manifest(std::string_view root, mr* mem = default_mr());

// 清理：删除 manifest 中记录的全部内容，最后删 marker；root 为空则一并删除。
void remove_farm(std::string_view root);

}  // namespace mol
