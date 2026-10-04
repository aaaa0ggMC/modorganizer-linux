#include "mol/loot.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

#include "mol/casefold.hpp"
#include "mol/http.hpp"

namespace mol::loot {
namespace fs = std::filesystem;
namespace {

[[noreturn]] void bad(const std::string& m) { throw Error("invalid_argument", "masterlist: " + m); }

std::string lower(std::string_view s) { return std::string(casefold(s)); }

// LOOT 的约定：名字里含 : \ * ? | 之一就是正则
bool looks_regex(const std::string& n) { return n.find_first_of(":\\*?|") != std::string::npos; }

std::string scalar(const YAML::Node& n) { return n.IsDefined() && n.IsScalar() ? n.as<std::string>() : std::string(); }

constexpr const char* kDefaultBranch = "v0.26";

}  // namespace

Masterlist parse_masterlist(std::string_view text) {
    YAML::Node root;
    try {
        root = YAML::Load(std::string(text));
    } catch (const YAML::Exception& e) {
        bad(std::string("invalid YAML: ") + e.what());
    }
    if (!root.IsMap()) bad("top level is not a map");
    Masterlist ml;
    // 分组：按 after 关系做拓扑排序，同层保持定义顺序
    std::vector<std::string> names;
    std::map<std::string, std::vector<std::string>> after;
    if (const auto g = root["groups"]; g && g.IsSequence()) {
        for (const auto& e : g) {
            const std::string n = scalar(e["name"]);
            if (n.empty()) continue;
            names.push_back(n);
            if (const auto a = e["after"]; a && a.IsSequence())
                for (const auto& x : a) after[n].push_back(scalar(x));
        }
    }
    if (std::find(names.begin(), names.end(), ml.default_group) == names.end()) names.push_back(ml.default_group);
    std::set<std::string> placed;
    while (placed.size() < names.size()) {
        bool progress = false;
        for (const auto& n : names) {
            if (placed.count(n)) continue;
            bool ready = true;
            for (const auto& a : after[n]) if (!placed.count(a) && std::find(names.begin(), names.end(), a) != names.end() && a != n) { ready = false; break; }
            if (ready) { placed.insert(n); ml.group_order.push_back(n); progress = true; break; }
        }
        if (!progress)  // 成环：按定义顺序放剩下的
            for (const auto& n : names) if (!placed.count(n)) { placed.insert(n); ml.group_order.push_back(n); }
    }
    for (std::size_t i = 0; i < ml.group_order.size(); ++i) ml.group_rank[ml.group_order[i]] = static_cast<int>(i);

    if (const auto p = root["plugins"]; p && p.IsSequence()) {
        for (const auto& e : p) {
            Rule r;
            r.name = scalar(e["name"]);
            if (r.name.empty()) continue;
            r.is_regex = looks_regex(r.name);
            r.group = scalar(e["group"]);
            for (const char* key : {"after", "req"}) {
                const auto a = e[key];
                if (!a || !a.IsSequence()) continue;
                for (const auto& x : a) {
                    if (x.IsScalar()) r.after.push_back(x.as<std::string>());
                    else if (x.IsMap() && !x["condition"]) r.after.push_back(scalar(x["name"]));  // 带条件的忽略
                }
            }
            // 没有任何有用信息的条目（只有 url/clean/msg 等）不保留，省内存
            if (r.group.empty() && r.after.empty()) continue;
            ml.rules.push_back(std::move(r));
        }
    }
    return ml;
}

std::string fetch_masterlist(std::string_view branch, std::string_view cache_dir, bool force) {
    fs::path dir;
    if (!cache_dir.empty()) dir = std::string(cache_dir);
    else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) dir = fs::path(x) / "mo-linux/loot";
    else dir = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".cache/mo-linux/loot";
    const std::string br = branch.empty() ? std::string(kDefaultBranch) : std::string(branch);
    const fs::path file = dir / ("skyrimse-" + br + ".yaml");
    std::error_code ec;
    auto read = [&] {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    if (!force && fs::is_regular_file(file, ec)) {
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(file, ec);
        if (!ec && age < std::chrono::hours(24)) return read();
    }
    try {
        HttpResponse r = http_get("https://raw.githubusercontent.com/loot/skyrimse/" + br + "/masterlist.yaml", {}, 60);
        if (r.status == 404 && branch.empty())
            r = http_get("https://raw.githubusercontent.com/loot/skyrimse/master/masterlist.yaml", {}, 60);
        if (r.status < 200 || r.status >= 300) throw Error("network_error", "HTTP " + std::to_string(r.status) + " fetching the LOOT masterlist");
        fs::create_directories(dir, ec);
        const fs::path tmp = file.string() + ".tmp";
        { std::ofstream os(tmp, std::ios::binary | std::ios::trunc); os << std::string(r.body); }
        fs::rename(tmp, file, ec);
        return std::string(r.body);
    } catch (const Error&) {
        if (fs::is_regular_file(file, ec)) return read();  // 旧缓存也比没有强
        throw;
    }
}

SortReport sort_with_masterlist(PluginList& list, const Masterlist& ml) {
    SortReport rep;
    const std::size_t n = list.rows.size();
    std::vector<std::string> names(n);
    std::map<std::string, std::size_t> by_name;
    for (std::size_t i = 0; i < n; ++i) { names[i] = lower(list.rows[i].name); by_name[names[i]] = i; }

    // 规则匹配：精确名 → 直接查表；正则 → 逐个插件匹配
    std::vector<std::vector<std::size_t>> hard_pred(n);  // hard_pred[i] = 必须排在 i 之前的插件
    std::vector<int> grank(n, ml.group_rank.count(ml.default_group) ? ml.group_rank.at(ml.default_group) : 0);
    std::vector<bool> has_group(n, false);
    auto match_all = [&](const std::string& pattern, bool is_regex) {
        std::vector<std::size_t> out;
        if (!is_regex) {
            auto it = by_name.find(lower(pattern));
            if (it != by_name.end()) out.push_back(it->second);
            return out;
        }
        try {
            const std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
            for (std::size_t i = 0; i < n; ++i) if (std::regex_match(std::string(list.rows[i].name), re)) out.push_back(i);
        } catch (const std::regex_error&) {}
        return out;
    };
    for (const auto& r : ml.rules) {
        const auto targets = match_all(r.name, r.is_regex);
        if (targets.empty()) continue;
        for (std::size_t t : targets) {
            if (!r.group.empty()) {
                if (auto it = ml.group_rank.find(r.group); it != ml.group_rank.end()) { grank[t] = it->second; has_group[t] = true; }
            }
            for (const auto& a : r.after)
                for (std::size_t p : match_all(a, looks_regex(a))) {
                    if (p == t) continue;
                    hard_pred[t].push_back(p);
                    ++rep.rules_applied;
                }
        }
    }
    for (bool g : has_group) if (g) ++rep.grouped;
    // masters 也是硬边
    for (std::size_t i = 0; i < n; ++i)
        for (const auto& m : list.rows[i].masters)
            if (auto it = by_name.find(lower(m)); it != by_name.end() && it->second != i) hard_pred[i].push_back(it->second);
    // 强制插件、ESM 区的先后次序由 normalize 规则决定：区号高的不能排到区号低的前面
    auto zone = [&](std::size_t i) { return list.rows[i].forced ? 0 : (list.rows[i].master ? 1 : 2); };
    // 与区规则冲突的硬边（例如 ESM 被要求排在某个 ESP 之后）无法满足，丢弃并记录
    for (std::size_t i = 0; i < n; ++i) {
        auto& v = hard_pred[i];
        v.erase(std::remove_if(v.begin(), v.end(), [&](std::size_t p) {
            if (zone(p) > zone(i)) { rep.cycles.push_back(std::string(list.rows[p].name) + " cannot load before " + std::string(list.rows[i].name) + " (ESM/ESP region rule)"); return true; }
            return false;
        }), v.end());
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }

    // 稳定 Kahn：就绪集合里取 (区, 组名次, 原位置) 最小者；遇环时放弃最靠前的未完成者的前置约束
    std::vector<bool> done(n, false);
    std::vector<std::size_t> order;
    order.reserve(n);
    while (order.size() < n) {
        std::size_t pick = n;
        auto better = [&](std::size_t a, std::size_t b) {
            if (zone(a) != zone(b)) return zone(a) < zone(b);
            if (grank[a] != grank[b]) return grank[a] < grank[b];
            return a < b;
        };
        for (std::size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            bool ready = true;
            for (std::size_t p : hard_pred[i]) if (!done[p]) { ready = false; break; }
            if (ready && (pick == n || better(i, pick))) pick = i;
        }
        if (pick == n) {  // 成环
            for (std::size_t i = 0; i < n; ++i) if (!done[i] && (pick == n || better(i, pick))) pick = i;
            rep.cycles.push_back("dependency cycle involving " + std::string(list.rows[pick].name) + "; its requirements were ignored");
        }
        done[pick] = true;
        order.push_back(pick);
    }
    std::vector<PluginRow> out;
    out.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        if (order[k] != k) rep.changed = true;
        out.push_back(list.rows[order[k]]);
    }
    list.rows.assign(out.begin(), out.end());
    return rep;
}

}  // namespace mol::loot
