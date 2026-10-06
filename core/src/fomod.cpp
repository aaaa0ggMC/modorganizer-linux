#include "mol/fomod.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "mol/casefold.hpp"

import alib6;

namespace mol::fomod {
namespace fs = std::filesystem;
namespace {

[[noreturn]] void bad(const std::string& m) { throw Error("invalid_argument", "FOMOD: " + m); }

std::string S(const string& s) { return std::string(s); }
std::string attr_or(const XmlNode& n, std::string_view k, std::string def = {}) {
    const string* v = n.attr(k);
    return v ? S(*v) : def;
}
std::string lower(std::string_view s) { return S(casefold(s)); }

PluginType parse_plugin_type(const std::string& s) {
    if (s == "Required") return PluginType::Required;
    if (s == "Optional") return PluginType::Optional;
    if (s == "Recommended") return PluginType::Recommended;
    if (s == "NotUsable") return PluginType::NotUsable;
    if (s == "CouldBeUsable") return PluginType::CouldBeUsable;
    return PluginType::Optional;  // MO2：未知类型按 Optional
}
GroupType parse_group_type(const std::string& s) {
    if (s == "SelectAtLeastOne") return GroupType::AtLeastOne;
    if (s == "SelectAtMostOne") return GroupType::AtMostOne;
    if (s == "SelectExactlyOne") return GroupType::ExactlyOne;
    if (s == "SelectAny") return GroupType::Any;
    if (s == "SelectAll") return GroupType::All;
    bad("unsupported group type " + s);
}

Cond parse_dependency_children(const XmlNode& n, bool and_op);

Cond parse_dep_node(const XmlNode& c) {
    Cond d;
    if (c.name == "fileDependency") {
        d.kind = Cond::Kind::File;
        d.name = attr_or(c, "file");
        d.value = attr_or(c, "state");
    } else if (c.name == "flagDependency") {
        d.kind = Cond::Kind::Flag;
        d.name = attr_or(c, "flag");
        d.value = attr_or(c, "value");
    } else if (c.name == "gameDependency" || c.name == "fommDependency" || c.name == "foseDependency") {
        d.kind = Cond::Kind::Version;
        d.name = c.name;
        d.value = attr_or(c, "version");
    } else if (c.name == "dependencies") {
        return parse_dependency_children(c, attr_or(c, "operator", "And") != "Or");
    } else {
        bad("unexpected element <" + S(c.name) + "> in dependencies");
    }
    return d;
}

Cond parse_dependency_children(const XmlNode& n, bool and_op) {
    Cond d;
    d.kind = Cond::Kind::Composite;
    d.op_and = and_op;
    for (const auto& c : n.children) d.children.push_back(parse_dep_node(c));
    return d;
}

std::vector<FileEntry> parse_files(const XmlNode& n) {
    std::vector<FileEntry> out;
    for (const auto& c : n.children) {
        if (c.name != "file" && c.name != "folder") bad("unexpected element <" + S(c.name) + "> in file list");
        FileEntry f;
        f.source = attr_or(c, "source");
        f.destination = attr_or(c, "destination");
        f.folder = c.name == "folder";
        const std::string p = attr_or(c, "priority", "0");
        try { f.priority = std::stoi(p); } catch (...) { f.priority = 0; }
        if (f.source.empty()) continue;  // 作者常用空 source 表示「什么都不装」
        out.push_back(std::move(f));
    }
    return out;
}

Plugin parse_plugin(const XmlNode& n) {
    Plugin p;
    p.name = attr_or(n, "name");
    for (const auto& c : n.children) {
        if (c.name == "description") p.description = S(c.text);
        else if (c.name == "image") p.image = attr_or(c, "path");
        else if (c.name == "files") p.files = parse_files(c);
        else if (c.name == "conditionFlags") {
            for (const auto& f : c.children)
                if (f.name == "flag") p.flags.emplace_back(attr_or(f, "name"), S(f.text));
        } else if (c.name == "typeDescriptor") {
            for (const auto& t : c.children) {
                if (t.name == "type") {
                    p.default_type = parse_plugin_type(attr_or(t, "name"));
                } else if (t.name == "dependencyType") {
                    for (const auto& x : t.children) {
                        if (x.name == "defaultType") p.default_type = parse_plugin_type(attr_or(x, "name"));
                        else if (x.name == "patterns") {
                            for (const auto& pat : x.children) {
                                Pattern pt;
                                if (const XmlNode* d = pat.child("dependencies")) pt.cond = parse_dep_node(*d);
                                if (const XmlNode* ty = pat.child("type")) pt.type = parse_plugin_type(attr_or(*ty, "name"));
                                p.patterns.push_back(std::move(pt));
                            }
                        }
                    }
                }
            }
        }
    }
    return p;
}

enum class Order { Ascending, Descending, Explicit };
Order order_of(const XmlNode& n) {
    const std::string o = attr_or(n, "order", "Ascending");
    if (o == "Ascending") return Order::Ascending;
    if (o == "Descending") return Order::Descending;
    if (o == "Explicit") return Order::Explicit;
    bad("unsupported order type " + o);
}
template <class T>
void sort_by_name(std::vector<T>& v, Order o) {
    if (o == Order::Ascending) std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) { return a.name < b.name; });
    else if (o == Order::Descending) std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) { return a.name > b.name; });
}

Step parse_step(const XmlNode& n) {
    Step s;
    s.name = attr_or(n, "name");
    for (const auto& c : n.children) {
        if (c.name == "visible") {
            s.has_visible = true;
            if (const XmlNode* d = c.child("dependencies")) s.visible = parse_dep_node(*d);
            else s.visible = parse_dependency_children(c, attr_or(c, "operator", "And") != "Or");
        } else if (c.name == "optionalFileGroups") {
            for (const auto& g : c.children) {
                if (g.name != "group") bad("unexpected element <" + S(g.name) + "> in optionalFileGroups");
                Group grp;
                grp.name = attr_or(g, "name");
                grp.type = parse_group_type(attr_or(g, "type"));
                if (const XmlNode* ps = g.child("plugins")) {
                    for (const auto& p : ps->children)
                        if (p.name == "plugin") grp.plugins.push_back(parse_plugin(p));
                    sort_by_name(grp.plugins, order_of(*ps));
                }
                s.groups.push_back(std::move(grp));
            }
        }
    }
    return s;
}

// ---- 版本比较（最多 4 段，缺省为 0）----
std::array<int, 4> parse_version(std::string_view v) {
    std::array<int, 4> a{0, 0, 0, 0};
    std::size_t i = 0;
    for (int k = 0; k < 4 && i < v.size(); ++k) {
        int val = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') val = val * 10 + (v[i++] - '0');
        a[static_cast<std::size_t>(k)] = val;
        if (i < v.size() && v[i] == '.') ++i; else break;
    }
    return a;
}

using FlagMap = std::map<std::string, std::string>;

bool test(const Cond& c, const FlagMap& flags, const Env& env) {
    switch (c.kind) {
        case Cond::Kind::Flag: {
            auto it = flags.find(c.name);
            if (it == flags.end()) return c.value.empty();
            return it->second == c.value;
        }
        case Cond::Kind::File: {
            const std::string st = env.file_state ? env.file_state(c.name) : "Missing";
            return st == c.value;
        }
        case Cond::Kind::Version: {
            const std::string& have = c.name == "gameDependency" ? env.game_version
                                      : c.name == "foseDependency" ? env.script_extender_version : std::string("0.13.21");
            return parse_version(c.value) <= parse_version(have);
        }
        case Cond::Kind::Composite: {
            if (c.children.empty()) return c.op_and;  // 空 And 恒真；空 Or 在 MO2 里为假
            for (const auto& ch : c.children) {
                const bool ok = test(ch, flags, env);
                if (!c.op_and && ok) return true;
                if (c.op_and && !ok) return false;
            }
            return c.op_and;
        }
    }
    return false;
}

PluginType effective_type(const Plugin& p, const FlagMap& flags, const Env& env) {
    for (const auto& pat : p.patterns)
        if (test(pat.cond, flags, env)) return pat.type;
    return p.default_type;
}

bool usable(PluginType t) { return t != PluginType::NotUsable; }

// 无显式选择时的默认选中。
std::set<std::size_t> default_selection(const Group& g, const std::vector<PluginType>& types) {
    std::set<std::size_t> sel;
    for (std::size_t i = 0; i < g.plugins.size(); ++i)
        if (types[i] == PluginType::Required) sel.insert(i);
    switch (g.type) {
        case GroupType::All:
            for (std::size_t i = 0; i < g.plugins.size(); ++i) if (usable(types[i])) sel.insert(i);
            break;
        case GroupType::Any:
            for (std::size_t i = 0; i < g.plugins.size(); ++i) if (types[i] == PluginType::Recommended) sel.insert(i);
            break;
        case GroupType::AtMostOne: {
            if (!sel.empty()) break;
            for (std::size_t i = 0; i < g.plugins.size(); ++i)
                if (types[i] == PluginType::Recommended) { sel.insert(i); break; }
            break;
        }
        case GroupType::ExactlyOne:
        case GroupType::AtLeastOne: {
            if (g.type == GroupType::AtLeastOne)
                for (std::size_t i = 0; i < g.plugins.size(); ++i) if (types[i] == PluginType::Recommended) sel.insert(i);
            if (!sel.empty()) break;
            for (std::size_t i = 0; i < g.plugins.size(); ++i)
                if (types[i] == PluginType::Recommended) { sel.insert(i); break; }
            if (sel.empty())
                for (std::size_t i = 0; i < g.plugins.size(); ++i) if (usable(types[i])) { sel.insert(i); break; }
            break;
        }
    }
    return sel;
}

std::string norm_slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// 逐级大小写不敏感地解析 rel 到 base 下已存在的路径；找不到返回空。拒绝 ".."。
fs::path find_ci(const fs::path& base, const std::string& rel_in) {
    fs::path cur = base;
    std::string rel = norm_slashes(rel_in);
    std::size_t i = 0;
    while (i <= rel.size()) {
        std::size_t j = rel.find('/', i);
        if (j == std::string::npos) j = rel.size();
        const std::string comp = rel.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") { if (j == rel.size()) break; continue; }
        if (comp == "..") bad("path escapes the archive: " + rel_in);
        const std::string want = lower(comp);
        fs::path next;
        std::error_code ec;
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec))
            if (lower(it->path().filename().string()) == want) { next = it->path(); break; }
        if (next.empty()) return {};
        cur = next;
        if (j == rel.size()) break;
    }
    return cur;
}

// 在 base 下为 rel 解析/创建目标路径：已有的目录沿用其大小写；返回最终路径。
fs::path dest_path(const fs::path& base, const std::string& rel_in) {
    fs::path cur = base;
    const std::string rel = norm_slashes(rel_in);
    std::size_t i = 0;
    while (i < rel.size()) {
        std::size_t j = rel.find('/', i);
        if (j == std::string::npos) j = rel.size();
        const std::string comp = rel.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") bad("destination escapes the mod directory: " + rel_in);
        const std::string want = lower(comp);
        fs::path pick = cur / comp;
        std::error_code ec;
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec))
            if (lower(it->path().filename().string()) == want) { pick = it->path(); break; }
        cur = pick;
    }
    return cur;
}

std::size_t copy_one(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) throw Error("io_error", "FOMOD: copy failed: " + ec.message(), dst.string());
    return 1;
}

std::size_t copy_tree(const fs::path& src, const fs::path& dst_dir) {
    std::size_t n = 0;
    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path d = dest_path(dst_dir, it->path().filename().string());
        if (it->is_directory(ec)) n += copy_tree(it->path(), d);
        else if (it->is_regular_file(ec)) n += copy_one(it->path(), d);
    }
    return n;
}

}  // namespace

const char* to_string(GroupType t) {
    switch (t) {
        case GroupType::AtLeastOne: return "SelectAtLeastOne";
        case GroupType::AtMostOne: return "SelectAtMostOne";
        case GroupType::ExactlyOne: return "SelectExactlyOne";
        case GroupType::Any: return "SelectAny";
        case GroupType::All: return "SelectAll";
    }
    return "SelectAny";
}
const char* to_string(PluginType t) {
    switch (t) {
        case PluginType::Required: return "Required";
        case PluginType::Optional: return "Optional";
        case PluginType::Recommended: return "Recommended";
        case PluginType::NotUsable: return "NotUsable";
        case PluginType::CouldBeUsable: return "CouldBeUsable";
    }
    return "Optional";
}

Config parse_config(const XmlNode& root) {
    if (root.name != "config") bad("root element must be <config>, got <" + S(root.name) + ">");
    Config cfg;
    for (const auto& c : root.children) {
        if (c.name == "moduleName") cfg.module_name = S(c.text);
        else if (c.name == "requiredInstallFiles") cfg.required = parse_files(c);
        else if (c.name == "installSteps") {
            for (const auto& s : c.children)
                if (s.name == "installStep") cfg.steps.push_back(parse_step(s));
            sort_by_name(cfg.steps, order_of(c));
        } else if (c.name == "conditionalFileInstalls") {
            if (const XmlNode* ps = c.child("patterns")) {
                for (const auto& pat : ps->children) {
                    CondInstall ci;
                    if (const XmlNode* d = pat.child("dependencies")) ci.cond = parse_dep_node(*d);
                    if (const XmlNode* f = pat.child("files")) ci.files = parse_files(*f);
                    cfg.conditional.push_back(std::move(ci));
                }
            }
        }
    }
    return cfg;
}

std::string norm_name(std::string_view s) {
    std::string u = std::string(html_unescape(s));
    const auto b = u.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    u = u.substr(b, u.find_last_not_of(" \t\r\n") - b + 1);
    return lower(u);
}

namespace {
// 在 map 里按键找：先精确，再按 norm_name
template <class M>
auto find_named(const M& m, const std::string& key) {
    auto it = m.find(key);
    if (it != m.end()) return it;
    const std::string n = norm_name(key);
    for (auto i = m.begin(); i != m.end(); ++i)
        if (norm_name(i->first) == n) return i;
    return m.end();
}
}  // namespace

std::string occurrence_key(std::string_view name, int k) {
    return k <= 1 ? std::string(name) : std::string(name) + " [#" + std::to_string(k) + "]";
}

Resolved resolve(const Config& cfg, const Choices& choices, bool use_defaults, const Env& env, std::vector<std::string>* lenient_notes) {
    Resolved out;
    FlagMap flags;
    std::vector<FileEntry> files = cfg.required;
    const bool lenient = lenient_notes != nullptr;
    std::map<std::string, int> step_seen;  // 可见步骤名（norm）→ 已出现次数

    for (const auto& step : cfg.steps) {
        StepState ss;
        ss.name = step.name;
        ss.key = step.name;
        ss.visible = !step.has_visible || test(step.visible, flags, env);
        if (!ss.visible) {
            out.steps.push_back(std::move(ss));
            continue;
        }
        FlagMap pending;  // 本步骤选中插件设置的标志，步骤结束后才生效
        std::vector<std::pair<std::string, std::string>> pending_flags;
        const int step_k = ++step_seen[norm_name(step.name)];
        ss.key = occurrence_key(step.name, step_k);
        const auto sit = find_named(choices, occurrence_key(step.name, step_k));
        std::map<std::string, int> group_seen;
        for (const auto& g : step.groups) {
            GroupState gs;
            gs.name = g.name;
            gs.type = g.type;
            std::vector<PluginType> types;
            for (const auto& p : g.plugins) types.push_back(effective_type(p, flags, env));

            std::set<std::size_t> sel;
            const std::set<std::string>* picked = nullptr;
            const int group_k = ++group_seen[norm_name(g.name)];
            gs.key = occurrence_key(g.name, group_k);
            if (sit != choices.end()) {
                auto git = find_named(sit->second, occurrence_key(g.name, group_k));
                if (git != sit->second.end()) picked = &git->second;
            }
            // 宽松模式：一处不合就记一句、整组退回默认
            bool fallback = false;
            const auto give_up = [&](const std::string& why) {
                if (!lenient) bad(why);
                lenient_notes->push_back(why + "; used the installer's default for this group");
                fallback = true;
            };
            if (picked) {
                gs.explicit_choice = true;
                for (const auto& nm : *picked) {
                    std::size_t idx = g.plugins.size();
                    for (std::size_t i = 0; i < g.plugins.size(); ++i)
                        if (g.plugins[i].name == nm) { idx = i; break; }
                    if (idx == g.plugins.size())
                        for (std::size_t i = 0; i < g.plugins.size(); ++i)
                            if (norm_name(g.plugins[i].name) == norm_name(nm)) { idx = i; break; }
                    if (idx == g.plugins.size()) {
                        const std::string why = "no plugin '" + nm + "' in group '" + g.name + "' of step '" + step.name + "'";
                        if (!lenient) bad(why);
                        lenient_notes->push_back(why + " (ignored)");
                        continue;
                    }
                    // NotUsable 插件在 MO2 里是灰掉且不勾选的（SelectAll 组也一样）；Vortex 记录的选择里可能带着它
                    // （例如只有说明文字、没有文件的「介绍」页）。按 MO2 语义忽略它，组的数量约束照常检查。
                    if (!usable(types[idx])) continue;
                    sel.insert(idx);
                }
                for (std::size_t i = 0; i < g.plugins.size(); ++i)
                    if (types[i] == PluginType::Required) sel.insert(i);
                const auto n = sel.size();
                if (g.type == GroupType::ExactlyOne && n != 1) give_up("group '" + g.name + "' needs exactly one choice");
                else if (g.type == GroupType::AtMostOne && n > 1) give_up("group '" + g.name + "' allows at most one choice");
                else if (g.type == GroupType::AtLeastOne && n < 1) give_up("group '" + g.name + "' needs at least one choice");
                else if (g.type == GroupType::All) {
                    std::size_t need = 0;
                    for (std::size_t i = 0; i < g.plugins.size(); ++i) if (usable(types[i])) ++need;
                    if (n != need) give_up("group '" + g.name + "' requires all of its plugins");
                }
                if (fallback) {
                    gs.explicit_choice = false;
                    sel = default_selection(g, types);
                }
            } else {
                if (!use_defaults && !lenient) bad("no choice given for group '" + g.name + "' of step '" + step.name + "'");
                if (!use_defaults && lenient)
                    lenient_notes->push_back("no recorded choice for group '" + g.name + "' of step '" + step.name + "'; used the installer's default");
                sel = default_selection(g, types);
            }
            for (std::size_t i = 0; i < g.plugins.size(); ++i) {
                PluginState ps;
                ps.name = g.plugins[i].name;
                ps.description = g.plugins[i].description;
                ps.image = g.plugins[i].image;
                ps.type = types[i];
                ps.selected = sel.count(i) > 0;
                if (ps.selected) {
                    for (const auto& f : g.plugins[i].files) files.push_back(f);
                    for (const auto& fl : g.plugins[i].flags) pending_flags.push_back(fl);
                }
                gs.plugins.push_back(std::move(ps));
            }
            ss.groups.push_back(std::move(gs));
        }
        for (const auto& [k, v] : pending_flags) flags[k] = v;  // 后设置者优先
        out.steps.push_back(std::move(ss));
    }
    for (const auto& ci : cfg.conditional)
        if (test(ci.cond, flags, env)) for (const auto& f : ci.files) files.push_back(f);

    std::stable_sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) { return a.priority < b.priority; });
    out.files = std::move(files);
    return out;
}

std::size_t install_files(const Resolved& r, std::string_view source_root, std::string_view dest_root, std::vector<std::string>* missing) {
    const fs::path src{std::string(source_root)}, dst{std::string(dest_root)};
    std::size_t n = 0;
    for (const auto& f : r.files) {
        const fs::path from = find_ci(src, f.source);
        std::error_code ec;
        if (from.empty() || !fs::exists(from, ec)) {
            if (missing) missing->push_back(f.source);
            continue;
        }
        if (f.folder) {
            n += copy_tree(from, dest_path(dst, f.destination));
        } else {
            std::string d = f.destination;
            const bool dir_like = !d.empty() && (d.back() == '/' || d.back() == '\\');
            const std::string base = from.filename().string();
            if (d.empty()) d = base;
            else if (dir_like) d += base;
            n += copy_one(from, dest_path(dst, d));
        }
    }
    return n;
}

std::string find_module_config(std::string_view root) {
    const fs::path r{std::string(root)};
    std::error_code ec;
    for (fs::directory_iterator it(r, ec), end; !ec && it != end; it.increment(ec)) {
        if (lower(it->path().filename().string()) != "fomod" || !it->is_directory(ec)) continue;
        for (fs::directory_iterator jt(it->path(), ec), jend; !ec && jt != jend; jt.increment(ec))
            if (lower(jt->path().filename().string()) == "moduleconfig.xml") return jt->path().string();
    }
    return {};
}

Config load_config(std::string_view path) {
    std::ifstream in{std::string(path), std::ios::binary};
    if (!in) throw Error("io_error", "cannot read ModuleConfig.xml", std::string(path));
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_config(parse_xml(bytes));
}

Choices parse_choices_json(std::string_view json) {
    alib6::AData doc(default_mr());
    if (!doc.load_from_memory(json) || !doc.is_object()) bad("choices file is not a JSON object");
    Choices out;
    const auto& top = doc.object();
    auto sit = top.find("steps");
    if (sit == top.end() || !sit.second().is_object()) bad("choices file needs a \"steps\" object");
    for (const auto& [step_name, step_val] : sit.second().object()) {
        if (!step_val.is_object()) bad("step '" + std::string(step_name) + "' must be an object");
        for (const auto& [group_name, arr] : step_val.object()) {
            if (!arr.is_array()) bad("group '" + std::string(group_name) + "' must be an array of plugin names");
            auto& set = out[std::string(step_name)][std::string(group_name)];
            for (const auto& p : arr.array()) {
                auto s = p.try_to<std::string_view>();
                if (!s) bad("plugin names must be strings");
                set.insert(std::string(*s));
            }
        }
    }
    return out;
}

std::string choices_to_json(const Choices& c) {
    alib6::AData doc(default_mr());
    auto& steps = doc["steps"];
    steps._set_object();
    for (const auto& [sn, groups] : c) {
        for (const auto& [gn, plugins] : groups) {
            auto& arr = steps[std::string_view(sn)][std::string_view(gn)];
            arr._set_array();
            std::ptrdiff_t i = 0;
            for (const auto& p : plugins) arr[i++] = std::string_view(p);
        }
    }
    alib6::JSON json{alib6::JSONConfig{.compact_lines = true, .compact_spaces = true, .sort_object = alib6::JSONConfig::sort_asc}};
    const auto text = doc.dump_to_string(json);
    return std::string(text.data(), text.size());
}

}  // namespace mol::fomod
