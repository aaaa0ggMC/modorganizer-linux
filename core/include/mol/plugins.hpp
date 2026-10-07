#pragma once
// 插件（.esp/.esm/.esl）加载顺序与启用状态：读写 profile 的 plugins.txt / loadorder.txt，读文件头里的 masters，做依赖检查与排序。
// 与 MO2 一致的约定：
//   * 可用插件 = 农场合并结果里 Data/ 顶层的 .esp/.esm/.esl（来自游戏本体、已启用 mod、overwrite，大小写不敏感）；
//   * 强制插件（游戏自带：Skyrim.esm、官方 DLC、Creation Club 清单等）永远启用，并排在最前；
//   * 带 ESM 标志（或扩展名为 .esm）的插件排在没有该标志的插件之前（游戏的硬性要求）；
//   * 列表里有、磁盘上已不存在的名字被丢弃；磁盘上有、列表里没有的追加到末尾并默认启用。
// 不做 LOOT 式的智能排序；sort_by_masters 只保证「每个插件排在它的 masters 之后」。
#include <string_view>

#include "mol/instance.hpp"
#include "mol/pmr.hpp"

namespace mol {

struct PluginHeader {
    using allocator_type = mol::allocator_type;
    bool master_flag = false;  // ESM 标志（记录头 flags & 0x1）
    bool light_flag = false;   // ESL 标志（flags & 0x200）
    vector<string> masters;
    explicit PluginHeader(allocator_type a = {}) : masters(a) {}
    PluginHeader(const PluginHeader& o, allocator_type a) : master_flag(o.master_flag), light_flag(o.light_flag), masters(o.masters, a) {}
    PluginHeader(PluginHeader&& o, allocator_type a) : master_flag(o.master_flag), light_flag(o.light_flag), masters(std::move(o.masters), a) {}
    PluginHeader(const PluginHeader&) = default;
    PluginHeader(PluginHeader&&) = default;
    PluginHeader& operator=(const PluginHeader&) = default;
    PluginHeader& operator=(PluginHeader&&) = default;
};

// 读 TES4 记录头。文件打不开 → Error{io_error}；不是 TES4 → Error{invalid_argument}。
PluginHeader read_plugin_header(std::string_view path, mr* mem = default_mr());

struct PluginRow {
    using allocator_type = mol::allocator_type;
    string name;
    string path;      // 真实文件
    string source;    // 提供它的层（"<game>" / mod 名 / "<overwrite>"）
    bool enabled = false;
    bool forced = false;
    bool master = false;  // ESM 标志或 .esm 扩展名
    bool light = false;
    vector<string> masters;
    explicit PluginRow(allocator_type a = {}) : name(a), path(a), source(a), masters(a) {}
    PluginRow(const PluginRow& o, allocator_type a) : name(o.name, a), path(o.path, a), source(o.source, a), enabled(o.enabled), forced(o.forced), master(o.master), light(o.light), masters(o.masters, a) {}
    PluginRow(PluginRow&& o, allocator_type a) : name(std::move(o.name), a), path(std::move(o.path), a), source(std::move(o.source), a), enabled(o.enabled), forced(o.forced), master(o.master), light(o.light), masters(std::move(o.masters), a) {}
    PluginRow(const PluginRow&) = default;
    PluginRow(PluginRow&&) = default;
    PluginRow& operator=(const PluginRow&) = default;
    PluginRow& operator=(PluginRow&&) = default;
};

struct PluginList {
    using allocator_type = mol::allocator_type;
    vector<PluginRow> rows;  // 加载顺序
    explicit PluginList(allocator_type a = {}) : rows(a) {}
    PluginList(const PluginList& o, allocator_type a) : rows(o.rows, a) {}
    PluginList(PluginList&& o, allocator_type a) : rows(std::move(o.rows), a) {}
    PluginList(const PluginList&) = default;
    PluginList(PluginList&&) = default;
    PluginList& operator=(const PluginList&) = default;
    PluginList& operator=(PluginList&&) = default;
};

// 游戏自带的强制插件（SSE）：host 不可用时的后备。
vector<string> default_forced_plugins(mr* mem = default_mr());

// forced：强制插件名（顺序即它们在列表最前面的顺序）；空 → default_forced_plugins。
PluginList load_plugins(const Instance& inst, std::span<const string> forced = {}, std::string_view profile = {}, mr* mem = default_mr());
// 写 profile 的 plugins.txt 与 loadorder.txt（原子），并各留一份 <名字>.mol-last-good（mo-linux 最后一次写的内容）。
void save_plugins(const Instance& inst, const PluginList& list, std::string_view profile = {});

// 游戏不管 plugins.txt 怎么写都会加载的插件（本体/DLC、_ResourcePack.esl、游戏目录 Skyrim.ccc 里的 CC）：
// 游戏自己重写 plugins.txt 时会省略它们，所以比较时要排除。
vector<string> implicit_plugins(const Instance& inst, mr* mem = default_mr());
// 在 .mol-last-good 里启用、现在 plugins.txt 里却没有启用的插件（不含 implicit_plugins）。
// 非空 = 别人（通常是游戏读到了别的列表后重写）把插件弄丢了。没有副本时返回空。
vector<string> plugins_lost_since_snapshot(const Instance& inst, std::string_view profile = {}, mr* mem = default_mr());
// 用 .mol-last-good 覆盖 plugins.txt / loadorder.txt（原子替换）。没有副本返回 false。
bool restore_plugins_snapshot(const Instance& inst, std::string_view profile = {});

// 修改（都会先 normalize，返回是否有变化）。名字大小写不敏感，找不到 → Error{mod_not_found}（沿用该码表示"没有这个对象"）。
// 禁用强制插件 → Error{invalid_argument}。
bool plugin_set_enabled(PluginList& list, std::string_view name, bool enabled);
// to_index：目标位置（0 起，越界夹到边界）。不会越过「强制/ESM 区」的边界：移动结果会被 normalize 修正。
bool plugin_move(PluginList& list, std::string_view name, std::size_t to_index);
// 保证每个插件排在它的 masters 之后（稳定；在同一 ESM/非 ESM 区内）。返回是否有变化。
bool plugin_sort_by_masters(PluginList& list);

struct MasterIssue {
    using allocator_type = mol::allocator_type;
    string plugin;
    string master;
    string kind;  // "missing"（磁盘上没有）| "disabled"（master 被禁用）| "after"（master 排在后面）
    explicit MasterIssue(allocator_type a = {}) : plugin(a), master(a), kind(a) {}
    MasterIssue(const MasterIssue& o, allocator_type a) : plugin(o.plugin, a), master(o.master, a), kind(o.kind, a) {}
    MasterIssue(MasterIssue&& o, allocator_type a) : plugin(std::move(o.plugin), a), master(std::move(o.master), a), kind(std::move(o.kind), a) {}
    MasterIssue(const MasterIssue&) = default;
    MasterIssue(MasterIssue&&) = default;
    MasterIssue& operator=(const MasterIssue&) = default;
    MasterIssue& operator=(MasterIssue&&) = default;
};
// 只检查已启用的插件。
vector<MasterIssue> check_masters(const PluginList& list, mr* mem = default_mr());

}  // namespace mol
