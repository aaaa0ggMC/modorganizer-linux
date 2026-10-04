#pragma once
// 近似 LOOT 的插件排序：用 LOOT 社区维护的 masterlist（loot/skyrimse）里的 after/req 规则与分组（groups）顺序，
// 加上插件自身的 masters 依赖，对加载顺序做稳定的拓扑排序。
//
// 与真正的 LOOT 的区别（有意简化）：不处理 condition 表达式（带条件的规则被忽略）、没有 userlist、没有 overlap/记录数启发式、
// 没有 metadata 的消息/脏插件报告。它只回答「在已知规则下，这个顺序合理吗」，足以让大多数整合包的加载顺序「差不多正确」。
//
// 排序规则：硬边（masters、masterlist 的 after/req）必须满足；在满足硬边的前提下，优先按分组顺序、其次保持当前顺序。
// 强制插件与 ESM 标志的分区规则（见 plugins.hpp）仍然最高优先。
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mol/plugins.hpp"

namespace mol::loot {

struct Rule {
    std::string name;       // 插件名（大小写不敏感）；is_regex 时是 ECMAScript 正则（整名匹配）
    bool is_regex = false;
    std::string group;      // 可空 → "default"
    std::vector<std::string> after;  // 无条件的 after/req（名字可能也是正则）
};

struct Masterlist {
    std::vector<std::string> group_order;               // 拓扑排序后的分组名（靠前者先加载）
    std::map<std::string, int> group_rank;               // 分组名 → 名次（0 起）
    std::vector<Rule> rules;
    std::string default_group = "default";
};

// 解析 YAML 文本；结构不对 → Error{invalid_argument}。
Masterlist parse_masterlist(std::string_view yaml_text);

// 取得 masterlist 文本：cache_dir 空 → ~/.cache/mo-linux/loot。缓存未过期（24 小时）直接用；联网失败退回旧缓存。
// branch 空 → 内置的默认分支。url 非空则只用这个地址。
std::string fetch_masterlist(std::string_view branch = {}, std::string_view cache_dir = {}, bool force = false);

struct SortReport {
    bool changed = false;
    std::size_t rules_applied = 0;   // 命中的 after/req 边数
    std::size_t grouped = 0;         // 有分组的插件数
    std::vector<std::string> cycles; // 因成环被忽略的规则说明
};

// 重排 list（同时保持强制/ESM 分区）。返回报告。
SortReport sort_with_masterlist(PluginList& list, const Masterlist& ml);

}  // namespace mol::loot
