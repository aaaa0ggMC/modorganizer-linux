#pragma once
// 大小写不敏感的多层目录合并（纯函数部分 + 扫描部分）。规则见 docs/PLAN.md §合并规则。
// 所有输出必须是确定性的：相同输入 → 相同输出。
#include <cstddef>
#include <span>
#include <string_view>

#include "mol/pmr.hpp"

namespace mol {

// 某一层的一个条目。rel 以 '/' 分隔，保留原始大小写，不以 '/' 开头。
// 例：层根 mods/A，文件 mods/A/Textures/Foo.dds，prefix="Data" → rel="Data/Textures/Foo.dds"
struct ScanEntry {
    using allocator_type = mol::allocator_type;
    string rel;
    bool is_dir = false;
    string abs;  // 真实路径（UTF-8）

    explicit ScanEntry(allocator_type a = {}) : rel(a), abs(a) {}
    ScanEntry(const ScanEntry& o, allocator_type a) : rel(o.rel, a), is_dir(o.is_dir), abs(o.abs, a) {}
    ScanEntry(ScanEntry&& o, allocator_type a) : rel(std::move(o.rel), a), is_dir(o.is_dir), abs(std::move(o.abs), a) {}
    ScanEntry(const ScanEntry&) = default;
    ScanEntry(ScanEntry&&) = default;
    ScanEntry& operator=(const ScanEntry&) = default;
    ScanEntry& operator=(ScanEntry&&) = default;
};

// 递归扫描 root，给每个条目的 rel 加上 prefix（可为空；非空如 "Data"）。
// 不为 prefix 本身生成目录条目，prefix 目录由 merge 按需隐式创建。
// 结果按 rel 字节序排序（确定性）。符号链接按指向目标的类型处理，目录链接不递归（防环）。
// root 不存在 → 返回空。
vector<ScanEntry> scan_layer(std::string_view root, std::string_view prefix = {}, mr* mem = default_mr());
// 同 scan_layer，但只列 root 的直接子项（不递归）。插件列表之类只关心顶层的场合用它，省掉整棵树的遍历。
vector<ScanEntry> scan_layer_top(std::string_view root, std::string_view prefix = {}, mr* mem = default_mr());

struct MergedEntry {
    using allocator_type = mol::allocator_type;
    string path;  // 规范路径（每一级大小写由"最先引入者"决定），'/' 分隔
    bool is_dir = false;
    std::size_t layer = 0;  // 文件=优先级最高的层；目录=最先引入的层
    string source;          // 胜出层中的真实路径（隐式创建的目录为空）

    explicit MergedEntry(allocator_type a = {}) : path(a), source(a) {}
    MergedEntry(const MergedEntry& o, allocator_type a) : path(o.path, a), is_dir(o.is_dir), layer(o.layer), source(o.source, a) {}
    MergedEntry(MergedEntry&& o, allocator_type a) : path(std::move(o.path), a), is_dir(o.is_dir), layer(o.layer), source(std::move(o.source), a) {}
    MergedEntry(const MergedEntry&) = default;
    MergedEntry(MergedEntry&&) = default;
    MergedEntry& operator=(const MergedEntry&) = default;
    MergedEntry& operator=(MergedEntry&&) = default;
};

struct Conflict {
    using allocator_type = mol::allocator_type;
    string path;                  // 规范路径（仅文件）
    std::size_t winner = 0;       // 胜出层
    vector<std::size_t> losers;   // 被覆盖的层，升序

    explicit Conflict(allocator_type a = {}) : path(a), losers(a) {}
    Conflict(const Conflict& o, allocator_type a) : path(o.path, a), winner(o.winner), losers(o.losers, a) {}
    Conflict(Conflict&& o, allocator_type a) : path(std::move(o.path), a), winner(o.winner), losers(std::move(o.losers), a) {}
    Conflict(const Conflict&) = default;
    Conflict(Conflict&&) = default;
    Conflict& operator=(const Conflict&) = default;
    Conflict& operator=(Conflict&&) = default;
};

struct Warning {
    using allocator_type = mol::allocator_type;
    string path;
    string message;  // 例如 "dir/file kind mismatch between layer 2 and 5"

    explicit Warning(allocator_type a = {}) : path(a), message(a) {}
    Warning(const Warning& o, allocator_type a) : path(o.path, a), message(o.message, a) {}
    Warning(Warning&& o, allocator_type a) : path(std::move(o.path), a), message(std::move(o.message), a) {}
    Warning(const Warning&) = default;
    Warning(Warning&&) = default;
    Warning& operator=(const Warning&) = default;
    Warning& operator=(Warning&&) = default;
};

struct MergeResult {
    using allocator_type = mol::allocator_type;
    vector<MergedEntry> entries;  // 按 path 字节序排序（含隐式目录）
    vector<Conflict> conflicts;   // 按 path 排序
    vector<Warning> warnings;

    explicit MergeResult(allocator_type a = {}) : entries(a), conflicts(a), warnings(a) {}
    MergeResult(const MergeResult& o, allocator_type a) : entries(o.entries, a), conflicts(o.conflicts, a), warnings(o.warnings, a) {}
    MergeResult(MergeResult&& o, allocator_type a) : entries(std::move(o.entries), a), conflicts(std::move(o.conflicts), a), warnings(std::move(o.warnings), a) {}
    MergeResult(const MergeResult&) = default;
    MergeResult(MergeResult&&) = default;
    MergeResult& operator=(const MergeResult&) = default;
    MergeResult& operator=(MergeResult&&) = default;
};

// layers[0] 优先级最低，layers.back() 最高。纯内存操作，不碰文件系统。
MergeResult merge_listings(std::span<const vector<ScanEntry>> layers, mr* mem = default_mr());

}  // namespace mol
