#include "mol/collection.hpp"

#include "mol/doctor.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#include "mol/casefold.hpp"
#include "mol/http.hpp"
#include "mol/md5.hpp"
#include "mol/mo2fmt.hpp"
#include "mol/mod_install.hpp"
#include "mol/parallel.hpp"
#include "mol/plugins.hpp"
#include "mol/reflink.hpp"
#include "mol/xxh64.hpp"

import alib6;

namespace mol::collection {
namespace fs = std::filesystem;
namespace {

[[noreturn]] void bad(const std::string& m) { throw Error("invalid_argument", "collection: " + m); }

std::string S(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return {};
    const auto& m = o.object();
    auto it = m.find(k);
    if (it == m.end()) return {};
    auto v = it.second().try_to<std::string_view>();
    return v ? std::string(*v) : std::string();
}
std::int64_t I(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return 0;
    const auto& m = o.object();
    auto it = m.find(k);
    if (it == m.end()) return 0;
    auto v = it.second().try_to<long long>();
    return v ? *v : 0;
}
bool B(const alib6::AData& o, const char* k, bool def = false) {
    if (!o.is_object()) return def;
    const auto& m = o.object();
    auto it = m.find(k);
    if (it == m.end()) return def;
    auto v = it.second().try_to<bool>();
    return v ? *v : def;
}
const alib6::AData* sub(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return nullptr;
    const auto& m = o.object();
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it.second();
}

RuleRef parse_ref(const alib6::AData& o) {
    RuleRef r;
    r.md5 = S(o, "fileMD5");
    r.logical_name = S(o, "logicalFileName");
    r.file_expression = S(o, "fileExpression");
    r.version_match = S(o, "versionMatch");
    return r;
}

std::string lower(std::string_view s) { return std::string(casefold(s)); }

bool ref_matches(const RuleRef& r, const Mod& m) {
    if (!r.md5.empty() && !m.source.md5.empty() && lower(r.md5) == lower(m.source.md5)) return true;
    if (!r.logical_name.empty() && lower(r.logical_name) == lower(m.source.logical_filename)) return true;
    return false;
}

std::string json_dump(const alib6::AData& d) {
    alib6::JSON json{alib6::JSONConfig{.dump_indent = 2, .compact_spaces = true, .sort_object = alib6::JSONConfig::sort_asc}};
    const auto t = d.dump_to_string(json);
    return std::string(t.data(), t.size()) + "\n";
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void atomic_write(const fs::path& target, const std::string& content) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) throw Error("io_error", "cannot write file", tmp.string());
        os << content;
        os.flush();
        if (!os) throw Error("io_error", "write failed", tmp.string());
    }
    fs::rename(tmp, target, ec);
    if (ec) throw Error("io_error", "cannot replace file: " + ec.message(), target.string());
}

}  // namespace

fomod::Choices choices_from_vortex(std::string_view json) {
    alib6::AData doc(default_mr());
    fomod::Choices out;
    if (!doc.load_from_memory(json) || !doc.is_object()) return out;
    if (S(doc, "type") != "fomod") return out;
    const auto* opts = sub(doc, "options");
    if (!opts || !opts->is_array()) return out;
    // 同名步骤/组（常见：每一步都叫 "Installation"、组名是 " "）按出现次序区分，见 fomod::occurrence_key；
    // 按名字直接合并会把不同步骤的选择混进同一组（"no plugin 'Finish Installation' in group ' '"、"needs exactly one choice"）
    std::map<std::string, int> step_seen;
    for (const auto& step : opts->array()) {
        const std::string raw = S(step, "name");
        const std::string sname = fomod::occurrence_key(raw, ++step_seen[fomod::norm_name(raw)]);
        const auto* groups = sub(step, "groups");
        if (!groups || !groups->is_array()) continue;
        std::map<std::string, int> group_seen;
        for (const auto& g : groups->array()) {
            const std::string graw = S(g, "name");
            auto& set = out[sname][fomod::occurrence_key(graw, ++group_seen[fomod::norm_name(graw)])];
            const auto* chs = sub(g, "choices");
            if (chs && chs->is_array())
                for (const auto& c : chs->array()) set.insert(S(c, "name"));
        }
    }
    return out;
}

Collection parse_collection(std::string_view json) {
    alib6::AData doc(default_mr());
    if (!doc.load_from_memory(json) || !doc.is_object()) bad("collection.json is not a JSON object");
    Collection c;
    if (const auto* info = sub(doc, "info")) {
        c.info.name = std::string(html_unescape(S(*info, "name")));
        c.info.author = S(*info, "author");
        c.info.domain = S(*info, "domainName");
        c.info.install_instructions = S(*info, "installInstructions");
        if (const auto* gv = sub(*info, "gameVersions"); gv && gv->is_array())
            for (const auto& v : gv->array()) if (auto s = v.try_to<std::string_view>()) c.info.game_versions.emplace_back(*s);
    }
    const auto* mods = sub(doc, "mods");
    if (!mods || !mods->is_array()) bad("missing \"mods\" array");
    for (const auto& m : mods->array()) {
        if (!m.is_object()) continue;
        Mod mod;
        mod.name = std::string(html_unescape(S(m, "name")));  // 清单里的名字是 HTML 转义过的
        mod.version = S(m, "version");
        mod.domain = S(m, "domainName");
        mod.optional = B(m, "optional");
        mod.phase = static_cast<int>(I(m, "phase"));
        if (const auto* s = sub(m, "source")) {
            mod.source.type = S(*s, "type");
            mod.source.mod_id = I(*s, "modId");
            mod.source.file_id = I(*s, "fileId");
            mod.source.file_size = I(*s, "fileSize");
            mod.source.md5 = S(*s, "md5");
            mod.source.logical_filename = S(*s, "logicalFilename");
            mod.source.url = S(*s, "url");
            mod.source.instructions = S(*s, "instructions");
            mod.source.tag = S(*s, "tag");
        }
        if (const auto* ch = sub(m, "choices"); ch && ch->is_object()) {
            alib6::JSON j{alib6::JSONConfig{.compact_lines = true, .compact_spaces = true}};
            const auto t = ch->dump_to_string(j);
            mod.choices = choices_from_vortex(std::string_view(t.data(), t.size()));
            mod.has_choices = true;
        }
        if (const auto* p = sub(m, "patches"); p && p->is_object() && !p->object().empty()) mod.has_patches = true;
        if (const auto* det = sub(m, "details")) mod.mod_type = lower(S(*det, "type"));
        if (const auto* h = sub(m, "hashes"); h && h->is_array())
            for (const auto& e : h->array())
                if (auto path = S(e, "path"); !path.empty()) mod.hashes.emplace_back(std::move(path), S(e, "md5"));
        if (mod.name.empty() && mod.source.logical_filename.empty()) continue;
        if (mod.name.empty()) mod.name = mod.source.logical_filename;
        c.mods.push_back(std::move(mod));
    }
    if (const auto* rules = sub(doc, "modRules"); rules && rules->is_array()) {
        for (const auto& r : rules->array()) {
            Rule rule;
            rule.type = S(r, "type");
            if (const auto* s = sub(r, "source")) rule.source = parse_ref(*s);
            if (const auto* f = sub(r, "reference")) rule.reference = parse_ref(*f);
            c.rules.push_back(std::move(rule));
        }
    }
    if (const auto* pl = sub(doc, "plugins"); pl && pl->is_array()) {
        c.has_plugins = true;
        for (const auto& p : pl->array()) {
            PluginSpec ps;
            ps.name = S(p, "name");
            ps.enabled = B(p, "enabled", true);
            if (!ps.name.empty()) c.plugins.push_back(std::move(ps));
        }
    }
    return c;
}

std::vector<std::size_t> install_order(const Collection& c) {
    std::vector<std::size_t> order(c.mods.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return c.mods[a].phase < c.mods[b].phase; });

    // 约束边 (x 必须在 y 之前)
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (const auto& r : c.rules) {
        if (r.type != "after" && r.type != "before") continue;
        std::vector<std::size_t> srcs, refs;
        for (std::size_t i = 0; i < c.mods.size(); ++i) {
            if (ref_matches(r.source, c.mods[i])) srcs.push_back(i);
            if (ref_matches(r.reference, c.mods[i])) refs.push_back(i);
        }
        for (auto s : srcs) for (auto f : refs) {
            if (s == f) continue;
            if (r.type == "after") edges.emplace_back(f, s);  // reference 先，source 后
            else edges.emplace_back(s, f);
        }
    }
    if (edges.empty()) return order;
    std::vector<bool> done(c.mods.size(), false);
    std::vector<std::size_t> out;
    out.reserve(order.size());
    while (out.size() < order.size()) {
        std::size_t pick = order.size();
        for (std::size_t k = 0; k < order.size(); ++k) {
            const std::size_t i = order[k];
            if (done[i]) continue;
            bool ready = true;
            for (const auto& [x, y] : edges)
                if (y == i && !done[x]) { ready = false; break; }
            if (ready) { pick = k; break; }
        }
        if (pick == order.size())  // 成环：忽略约束，取第一个未完成的
            for (std::size_t k = 0; k < order.size(); ++k) if (!done[order[k]]) { pick = k; break; }
        done[order[pick]] = true;
        out.push_back(order[pick]);
    }
    return out;
}

// ---- 状态 -----------------------------------------------------------------------------

std::string tool_for_generated_plugin(std::string_view plugin) {
    static const std::pair<std::string_view, std::string_view> known[] = {
        {"fnis.esp", "the behavior engine (Pandora Behaviour Engine, Nemesis or FNIS)"},
        {"nemesis pcea.esp", "Nemesis"},
        {"dyndolod.esp", "DynDOLOD (texgen + dyndolod)"},
        {"dyndolod.esm", "DynDOLOD (texgen + dyndolod)"},
        {"occlusion.esp", "xLODGen / DynDOLOD occlusion"},
        {"synthesis.esp", "Synthesis"},
        {"bashed patch, 0.esp", "Wrye Bash"},
        {"smashed patch.esp", "Mator Smash"},
    };
    const std::string l = lower(plugin);
    for (const auto& [n, t] : known) if (l == n) return std::string(t);
    return {};
}

std::string collection_dir(const Instance& inst, std::string_view slug) {
    return (fs::path(std::string(inst.root)) / "collections" / std::string(slug)).string();
}

State load_state(const Instance& inst, std::string_view slug) {
    State st;
    st.slug = std::string(slug);
    const fs::path f = fs::path(collection_dir(inst, slug)) / "state.json";
    std::error_code ec;
    if (!fs::exists(f, ec)) return st;
    alib6::AData doc(default_mr());
    if (!doc.load_from_memory(read_all(f)) || !doc.is_object())
        throw Error("config_invalid", "collection state file is corrupt (delete it to start over)", f.string());
    st.name = S(doc, "name");
    st.revision = I(doc, "revision");
    if (const auto* mods = sub(doc, "mods"); mods && mods->is_object())
        for (const auto& [k, v] : mods->object()) {
            ModState ms;
            ms.name = S(v, "name");
            ms.status = S(v, "status");
            ms.archive = S(v, "archive");
            ms.mod_dir = S(v, "mod_dir");
            ms.note = S(v, "note");
            ms.kind = S(v, "kind");
            ms.url = S(v, "url");
            st.mods[std::string(k)] = std::move(ms);
        }
    if (const auto* ov = sub(doc, "overrides"); ov && ov->is_object())
        for (const auto& [k, v] : ov->object()) {
            Override o;
            o.skip = B(v, "skip");
            o.fomod_defaults = B(v, "fomod_defaults");
            o.archive = S(v, "archive");
            o.reinstall = B(v, "reinstall");
            if (auto cj = S(v, "choices"); !cj.empty()) {
                o.choices = fomod::parse_choices_json(cj);
                o.has_choices = true;
            }
            st.overrides[std::string(k)] = std::move(o);
        }
    return st;
}

void save_state(const Instance& inst, const State& s) {
    alib6::AData doc(default_mr());
    doc["version"] = 1;
    doc["slug"] = std::string_view(s.slug);
    doc["name"] = std::string_view(s.name);
    doc["revision"] = static_cast<long long>(s.revision);
    auto& mods = doc["mods"];
    mods._set_object();
    for (const auto& [k, m] : s.mods) {
        auto& e = mods[std::string_view(k)];
        e["name"] = std::string_view(m.name);
        e["status"] = std::string_view(m.status);
        e["archive"] = std::string_view(m.archive);
        e["mod_dir"] = std::string_view(m.mod_dir);
        e["note"] = std::string_view(m.note);
        e["kind"] = std::string_view(m.kind);
        e["url"] = std::string_view(m.url);
    }
    auto& ov = doc["overrides"];
    ov._set_object();
    for (const auto& [k, o] : s.overrides) {
        auto& e = ov[std::string_view(k)];
        e["skip"] = o.skip;
        e["fomod_defaults"] = o.fomod_defaults;
        e["archive"] = std::string_view(o.archive);
        e["reinstall"] = o.reinstall;
        e["choices"] = o.has_choices ? fomod::choices_to_json(o.choices) : std::string();
    }
    atomic_write(fs::path(collection_dir(inst, s.slug)) / "state.json", json_dump(doc));
}

// ---- 流水线 ---------------------------------------------------------------------------
namespace {

// 下载目录里按「大小 + md5」找已有的压缩包（不需要联网）。
std::string find_cached(const fs::path& dl, const Source& s) {
    if (s.md5.empty() || s.file_size <= 0) return {};
    std::error_code ec;
    for (fs::directory_iterator it(dl, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto ext = lower(it->path().extension().string());
        if (ext == ".meta" || ext == ".part") continue;
        if (static_cast<std::int64_t>(it->file_size(ec)) != s.file_size) continue;
        if (lower(md5_file(it->path().string())) == lower(s.md5)) return it->path().string();
    }
    return {};
}

// 已装的 Nexus 文件：(modid, fileid) → 各个安装（mod 目录的绝对路径, FOMOD 选择指纹）。扫的是 mods/ 下的**全部**目录，
// 不只当前 profile 的 modlist——另一个 profile / 集合装的同一个文件也能复用（同一个实例的 mods/ 本来就是共享的）。
struct Installed {
    std::string path, fingerprint;
};
using NexusIndex = std::map<std::pair<std::int64_t, std::int64_t>, std::vector<Installed>>;
void index_mods_dir(NexusIndex& out, const fs::path& mods) {
    std::error_code ec;
    for (fs::directory_iterator it(mods, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) || it->path().filename().string().starts_with(".")) continue;
        const Ini meta = Ini::load((it->path() / "meta.ini").string());
        auto num = [&](const char* k) -> std::int64_t {
            const auto v = meta.get("General", k);
            if (!v || v->empty()) return 0;
            std::int64_t n = 0;
            for (char ch : *v) { if (ch < '0' || ch > '9') return 0; n = n * 10 + (ch - '0'); }
            return n;
        };
        const std::int64_t mid = num("modid"), fid = num("fileid");
        if (mid > 0 && fid > 0) out[{mid, fid}].push_back({it->path().string(), std::string(meta.get("General", "mol_fomod").value_or(""))});
    }
}

// FOMOD 选择的指纹（写进 meta.ini 的 mol_fomod；同一个 Nexus 文件只有选择相同才复用）：
// "choices:<xxh64>" | "replicate:<xxh64>"（按清单 hashes 复刻）| "defaults" | ""（没走 FOMOD，或不知道——MO2 / nexus install 装的）
std::string replicate_fingerprint(const std::vector<std::pair<std::string, std::string>>& hashes) {
    std::string all;
    for (const auto& [p, h] : hashes) all += p + '\0' + h + '\n';
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(xxh64(all)));
    return std::string("replicate:") + buf;
}
std::string fomod_fingerprint(const fomod::Choices* choices, bool defaults) {
    if (choices) {
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(xxh64(fomod::choices_to_json(*choices))));
        return std::string("choices:") + buf;
    }
    return defaults ? "defaults" : "";
}

// 清单里的 SKSE64 条目（外部来源）：按链接或名字认。
bool is_skse_entry(const Mod& m) {
    const std::string url = lower(m.source.url), name = lower(m.name);
    return url.find("silverlock.org") != std::string::npos || name.find("script extender") != std::string::npos ||
           name.starts_with("skse64") || name == "skse";
}

// 提供 skse64_loader.exe 的根目录型 mod 目录名；由游戏目录本身提供则为空。
std::string skse_mod_dir(const Instance& inst) {
    std::error_code ec;
    for (const auto& md : list_mods(inst)) {
        if (!md.enabled || !md.exists || !md.root) continue;
        for (fs::directory_iterator it(fs::path(std::string(md.path)), ec), end; !ec && it != end; it.increment(ec))
            if (lower(it->path().filename().string()) == "skse64_loader.exe") return fs::path(std::string(md.path)).filename().string();
    }
    return {};
}

std::string page_url(const Mod& m) {
    if (m.source.type == "nexus" && m.source.mod_id > 0)
        return "https://www.nexusmods.com/" + (m.domain.empty() ? std::string("skyrimspecialedition") : m.domain) + "/mods/" +
               std::to_string(m.source.mod_id) + "?tab=files&file_id=" + std::to_string(m.source.file_id);
    return m.source.url;
}

void ensure_profile(const Instance& inst, const std::string& name) {
    const fs::path d = fs::path(std::string(inst.profiles_dir)) / name;
    std::error_code ec;
    std::string actual;
    for (fs::directory_iterator it(fs::path(std::string(inst.profiles_dir)), ec), end; !ec && it != end; it.increment(ec))
        if (lower(it->path().filename().string()) == lower(name) && it->is_directory(ec)) return;
    fs::create_directories(d, ec);
    if (ec) throw Error("io_error", "cannot create profile directory: " + ec.message(), d.string());
    if (!fs::exists(d / "modlist.txt", ec)) std::ofstream(d / "modlist.txt") << "# This file was automatically generated by Mod Organizer.\n";
}


struct Fetched {
    std::string path, err_code, err_msg;
};

// 下载一个 mod 的压缩包（nexus 直连 / direct）。线程安全：只读 client，写自己的返回值。
Fetched fetch_one(const Instance& inst, const NexusClient* client, const Mod& m, const std::function<void(std::uint64_t, std::uint64_t)>& prog) {
    Fetched f;
    const fs::path downloads{std::string(inst.downloads_dir)};
    try {
        if (m.source.type == "nexus") {
            if (!client) { f.err_code = "offline"; f.err_msg = "offline: the archive is not in the downloads folder"; return f; }
            const std::string domain = m.domain.empty() ? std::string(nexus_game_domain(inst.cfg.game)) : m.domain;
            const auto dl = nexus_download(*client, inst.downloads_dir, domain, m.source.mod_id, m.source.file_id, nullptr,
                                           [&](std::uint64_t d, std::uint64_t t) { if (prog) prog(d, t); return true; });
            f.path = std::string(dl.path);
        } else if (m.source.type == "direct" && !m.source.url.empty()) {
            std::string_view u = m.source.url;
            if (auto q = u.find_first_of("?#"); q != std::string_view::npos) u = u.substr(0, q);
            std::string base(u.substr(u.rfind('/') == std::string_view::npos ? 0 : u.rfind('/') + 1));
            if (base.empty()) base = m.name;
            for (char& ch : base) if (ch == '/' || ch == '\\') ch = '_';
            const std::string dest = (downloads / base).string();
            http_download(m.source.url, dest, {}, [&](std::uint64_t d, std::uint64_t t) { if (prog) prog(d, t); return true; });
            f.path = dest;
        }
    } catch (const Error& e) {
        f.err_code = e.code;
        f.err_msg = e.what();
    }
    return f;
}

}  // namespace

Report install_collection(const Instance& inst, const NexusClient* client, const Collection& c, State& state, const InstallOptions& opt,
                          std::string_view game_version) {
    Report rep;
    const std::string profile = opt.profile.empty() ? std::string(inst.cfg.profile) : opt.profile;
    ensure_profile(inst, profile);
    const fs::path downloads{std::string(inst.downloads_dir)};
    std::error_code ec;
    fs::create_directories(downloads, ec);

    if (!c.info.game_versions.empty() && !game_version.empty()) {
        bool match = false;
        for (const auto& v : c.info.game_versions) if (v == game_version) match = true;
        if (!match) rep.notes.push_back("the collection targets game version " + c.info.game_versions.front() + " but this game is " + std::string(game_version));
    }
    for (const auto& r : c.rules)
        if (r.type == "requires" || r.type == "conflicts") rep.notes.push_back("collection rule '" + r.type + "' is not enforced by mo-linux");

    const auto order = install_order(c);
    std::vector<string> final_names;  // 低→高优先级
    std::size_t step = 0;

    std::optional<NexusIndex> nexus_files;  // 首次用到时建：本实例 mods/ 下全部目录
    NexusIndex other_files;                 // opt.reuse_from 里别的实例的 mods/
    for (const auto& d : opt.reuse_from) index_mods_dir(other_files, fs::path(d));
    // FOMOD 的 fileDependency：清单插件列表里启用的插件按 Active 算——那是策展人装完后的环境；
    // 按安装顺序，被依赖的 mod 可能排在后面、此刻还没装上（例：Skyrim Unbound 的 Bruma 选项要 BSHeartland.esm）
    std::set<std::string> collection_plugins;
    for (const auto& p : c.plugins) if (p.enabled) collection_plugins.insert(lower(p.name));
    // 清单 hashes 的复刻优先于 FOMOD 默认；用户/清单给了具体选择则按选择
    auto replicates = [&](const Mod& m, const Override& ov) { return !m.hashes.empty() && !ov.has_choices && !m.has_choices; };
    auto wanted_fingerprint = [&](const Mod& m, const Override& ov) {
        if (replicates(m, ov)) return replicate_fingerprint(m.hashes);
        const fomod::Choices* want_choices = ov.has_choices ? &ov.choices : m.has_choices ? &m.choices : nullptr;
        return fomod_fingerprint(want_choices, ov.fomod_defaults || opt.fomod_defaults);
    };
    // 这个 mod 能不能直接用已有的安装（本实例任一目录 / 别的实例）：能就不必下载
    auto compatible_fp = [&](const Mod& m, const Override& ov, const std::string& have) {
        const std::string want = wanted_fingerprint(m, ov);
        // 想要具体选择/复刻 → 指纹必须相同；想要默认/不需要选择 → 对方没记指纹（多半没 FOMOD）或也是默认即可
        if (want != "defaults" && !want.empty()) return have == want;
        return have.empty() || have == "defaults";
    };
    auto find_reusable = [&](const NexusIndex& idx, const Mod& m, const Override& ov) -> const Installed* {
        if (!ov.archive.empty() || m.source.type != "nexus" || m.source.mod_id <= 0 || m.source.file_id <= 0) return nullptr;
        const auto hit = idx.find({m.source.mod_id, m.source.file_id});
        if (hit == idx.end()) return nullptr;
        for (const auto& i : hit->second) if (compatible_fp(m, ov, i.fingerprint)) return &i;
        return nullptr;
    };
    // 去重的参考：同一个文件的别的安装（选择不同）在前，同一个 mod 的别的版本（没改过的贴图/模型常常一字不差）在后
    auto any_install_of = [&](const Mod& m) -> std::vector<const Installed*> {
        std::vector<const Installed*> same_file, same_mod;
        for (const NexusIndex* idx : {&*nexus_files, &other_files})
            for (auto it = idx->lower_bound({m.source.mod_id, 0}); it != idx->end() && it->first.first == m.source.mod_id; ++it)
                for (const auto& i : it->second) (it->first.second == m.source.file_id ? same_file : same_mod).push_back(&i);
        same_file.insert(same_file.end(), same_mod.begin(), same_mod.end());
        return same_file;
    };
    nexus_files = NexusIndex{};
    index_mods_dir(*nexus_files, fs::path(std::string(inst.mods_dir)));

    // ---- 预取阶段：把需要联网下载的压缩包并行下完（安装仍按顺序串行进行）----
    std::map<std::string, std::string> cached;    // key → 本地已有的压缩包（不需要下载）
    std::map<std::string, Fetched> fetched;       // key → 并行下载的结果
    {
        // download 进度的 total 恒为「本次要装的 mod 的文件总大小」（跳过的、未选的可选 mod 不算），
        // 已经在手的（装好的、下载过的、用户给的压缩包）从一开始就算进 done：首次运行与中断续跑的 total 相同。
        std::vector<std::size_t> todo;
        std::uint64_t total = 0, have = 0;
        auto size_of = [](const Mod& m) { return static_cast<std::uint64_t>(std::max<std::int64_t>(m.source.file_size, 0)); };
        for (const std::size_t idx : order) {
            const Mod& m = c.mods[idx];
            const std::string key = m.key();
            const Override ov = state.overrides.count(key) ? state.overrides.at(key) : Override{};
            if (ov.skip || (m.optional && !opt.include_optional)) continue;
            total += size_of(m);
            const auto sit = state.mods.find(key);
            if (sit != state.mods.end() && sit->second.status == "installed" && !sit->second.mod_dir.empty() &&
                fs::is_directory(fs::path(std::string(inst.mods_dir)) / sit->second.mod_dir, ec)) {
                have += size_of(m);
                continue;
            }
            if (!ov.archive.empty()) { have += size_of(m); continue; }
            if (m.has_patches) continue;
            if (!ov.reinstall && (find_reusable(*nexus_files, m, ov) || find_reusable(other_files, m, ov))) { have += size_of(m); continue; }  // 不用下载
            if (sit != state.mods.end() && !sit->second.archive.empty() && fs::is_regular_file(sit->second.archive, ec)) { have += size_of(m); continue; }
            if (auto p = find_cached(downloads, m.source); !p.empty()) { cached[key] = p; have += size_of(m); continue; }
            if (client && (m.source.type == "nexus" || (m.source.type == "direct" && !m.source.url.empty()))) todo.push_back(idx);
        }
        if (!todo.empty()) {
            if (opt.progress) opt.progress("download", "", have, total);
            std::vector<std::atomic<std::uint64_t>> cur(todo.size());
            std::mutex mu;
            std::size_t finished = 0;
            parallel_for(todo.size(), download_jobs(opt.jobs), [&](std::size_t i) {
                const Mod& m = c.mods[todo[i]];
                Fetched f = fetch_one(inst, client, m, [&](std::uint64_t d, std::uint64_t) {
                    cur[i].store(d);
                    if (!opt.progress) return;
                    std::uint64_t sum = have;
                    for (auto& x : cur) sum += x.load();
                    std::lock_guard<std::mutex> g(mu);
                    opt.progress("download", "", sum, total);
                });
                std::lock_guard<std::mutex> g(mu);
                fetched[m.key()] = std::move(f);
                if (opt.progress) opt.progress("downloaded", m.name, ++finished, todo.size());
            });
        }
    }

    for (const std::size_t idx : order) {
        const Mod& m = c.mods[idx];
        const std::string key = m.key();
        ++step;
        if (opt.progress) opt.progress("install", m.name, step, order.size());

        ModOutcome out;
        out.key = key;
        out.name = m.name;
        ModState& ms = state.mods[key];
        ms.name = m.name;
        ms.kind.clear();
        ms.url = page_url(m);
        const Override ov = state.overrides.count(key) ? state.overrides.at(key) : Override{};
        auto pend = [&](const char* kind, const std::string& detail) {
            Pending p;
            p.key = key; p.name = m.name; p.kind = kind; p.detail = detail; p.url = page_url(m);
            rep.pending.push_back(std::move(p));
            ms.status = "pending";
            ms.note = detail;
            ms.kind = kind;
            out.status = "pending";
            out.note = detail;
        };
        auto fail = [&](const std::string& note) {
            ms.status = "failed";
            ms.note = note;
            out.status = "failed";
            out.note = note;
            ++rep.failed;
        };
        auto finish = [&] {
            rep.mods.push_back(out);
            save_state(inst, state);
        };

        // 已完成；但装的内容与现在要的不同时原地重装：用户要求重装，或清单要求复刻而旧版本是按 FOMOD 默认装的
        bool reinstall = false;
        if (ms.status == "installed" && !ms.mod_dir.empty() && fs::is_directory(fs::path(std::string(inst.mods_dir)) / ms.mod_dir, ec)) {
            if (ov.reinstall) reinstall = true;
            else if (replicates(m, ov) && ov.archive.empty()) {
                const Ini meta = Ini::load((fs::path(std::string(inst.mods_dir)) / ms.mod_dir / "meta.ini").string());
                reinstall = std::string(meta.get("General", "mol_fomod").value_or("")) != replicate_fingerprint(m.hashes);
            }
        }
        if (!reinstall && ms.status == "installed" && !ms.mod_dir.empty() && fs::is_directory(fs::path(std::string(inst.mods_dir)) / ms.mod_dir, ec)) {
            out.status = "installed";
            out.mod_dir = ms.mod_dir;
            ++rep.installed;
            final_names.emplace_back(ms.mod_dir);
            rep.mods.push_back(out);
            if (m.source.type == "nexus" && m.source.mod_id > 0) {  // 早期版本装的没有来源信息：补写（幂等）
                const std::string md = (fs::path(std::string(inst.mods_dir)) / ms.mod_dir).string();
                set_mod_meta(md, "gameName", m.domain.empty() ? std::string(nexus_game_domain(inst.cfg.game)) : m.domain);
                set_mod_meta(md, "modid", std::to_string(m.source.mod_id));
                set_mod_meta(md, "fileid", std::to_string(m.source.file_id));
                if (!m.version.empty()) set_mod_meta(md, "version", m.version);
            }
            if (m.deploys_to_root()) {  // 早期版本把 ENB 预设之类装成了普通 mod（落到 Data/ 下，ENB 找不到）：布局相同，补上根目录标记即可
                const std::string md = (fs::path(std::string(inst.mods_dir)) / ms.mod_dir).string();
                if (Ini::load(md + "/meta.ini").get("General", "mol_root").value_or("") != "true") {
                    mark_mod_root(md, true);
                    rep.notes.push_back(m.name + ": marked as a root-folder mod (Vortex type '" + m.mod_type + "' deploys to the game folder)");
                }
            }
            continue;
        }
        if (ov.skip || (m.optional && !opt.include_optional)) {
            ms.status = "skipped";
            ms.note = ov.skip ? "skipped by the user" : "optional mod skipped";
            out.status = "skipped";
            out.note = ms.note;
            ++rep.skipped;
            finish();
            continue;
        }
        if (m.has_patches && ov.archive.empty()) {
            pend("unsupported", "this mod needs binary patches from the collection, which mo-linux cannot apply; provide an archive with `collection resolve --archive` or skip it");
            finish();
            continue;
        }

        // 同一个 Nexus 文件已经装在实例里（另一个集合、`nexus install` 或 MO2 装的，meta.ini 的 modid/fileid 一致）→ 复用，不再装一份
        // 选择不同就不复用（否则两个集合/两个 profile 选了不同 FOMOD 选项会串味）
        if (!reinstall) {
            if (const Installed* hit = find_reusable(*nexus_files, m, ov)) {
                const std::string dir = fs::path(hit->path).filename().string();
                ms.status = "installed";
                ms.mod_dir = dir;
                ms.note = "already installed (same Nexus file)";
                out.status = "installed";
                out.mod_dir = dir;
                out.note = ms.note;
                ++rep.installed;
                ++rep.reused;
                final_names.emplace_back(dir);
                finish();
                continue;
            }
            // 别的实例里有：reflink 整个目录过来（同一个支持 reflink 的文件系统上瞬间完成、不占空间；否则照常安装）
            if (const Installed* hit = find_reusable(other_files, m, ov)) {
                std::string dir = ms.mod_dir.empty() ? m.name : ms.mod_dir;
                if (ms.mod_dir.empty() && mod_name_taken(inst, dir, profile)) dir += " [" + (m.source.tag.empty() ? std::to_string(idx) : m.source.tag) + "]";
                dir = sanitize_mod_name(dir);
                const fs::path target = fs::path(std::string(inst.mods_dir)) / dir;
                if (!fs::exists(fs::symlink_status(target, ec)) && reflink_tree(hit->path, target)) {
                    nexus_files->operator[]({m.source.mod_id, m.source.file_id}).push_back({target.string(), hit->fingerprint});
                    ms.status = "installed";
                    ms.mod_dir = dir;
                    ms.note = "reflinked from " + hit->path;
                    out.status = "installed";
                    out.mod_dir = dir;
                    out.note = ms.note;
                    ++rep.installed;
                    ++rep.reflinked;
                    final_names.emplace_back(dir);
                    finish();
                    continue;
                }
            }
        }

        // SKSE64 在清单里通常是外部来源（skse.silverlock.org），不能从 Nexus 自动下载；
        // mo-linux 有 `skse install`（从 Nexus 的 SKSE 页面挑与游戏版本匹配的构建），已装就算满足。
        if (ov.archive.empty() && m.source.type != "nexus" && is_skse_entry(m)) {
            if (root_provides(inst, "skse64_loader.exe")) {
                ms.status = "installed";
                ms.mod_dir = skse_mod_dir(inst);
                ms.note = "provided by the installed SKSE64";
                out.status = "installed";
                out.mod_dir = ms.mod_dir;
                out.note = ms.note;
                ++rep.installed;
                if (!ms.mod_dir.empty()) final_names.emplace_back(ms.mod_dir);
                finish();
                continue;
            }
            pend("skse", "SKSE64 is distributed outside Nexus: run `mo-linux skse install` (it picks the build that matches your game), then run `collection install` again");
            finish();
            continue;
        }

        // 1) 取得压缩包
        std::string archive = ov.archive;
        bool user_archive = !archive.empty();
        if (user_archive && !fs::is_regular_file(archive, ec)) {
            fail("the archive given with `collection resolve` no longer exists: " + archive);
            finish();
            continue;
        }
        if (archive.empty() && !ms.archive.empty() && fs::is_regular_file(ms.archive, ec)) archive = ms.archive;  // 上一轮已下载（例如在等 FOMOD 选择）
        if (archive.empty()) { auto it = cached.find(key); if (it != cached.end()) archive = it->second; }
        if (archive.empty()) {
            const std::string& t = m.source.type;
            if (t == "nexus" || (t == "direct" && !m.source.url.empty())) {
                Fetched f;
                if (auto it = fetched.find(key); it != fetched.end()) f = it->second;
                else f = fetch_one(inst, client, m, [&](std::uint64_t d, std::uint64_t tt) { if (opt.progress) opt.progress("download", m.name, d, tt); });
                if (!f.path.empty()) archive = f.path;
                else if (f.err_code == "offline") pend("manual_download", f.err_msg);
                else if (f.err_code == "nexus_premium")
                    pend("manual_download", "a free Nexus account cannot download this directly: open the page, click 'Slow download' / 'Mod Manager Download', then run `collection resolve --mod KEY --nxm <the nxm:// link>`");
                else fail(f.err_code + ": " + f.err_msg);
            } else if (t == "bundle") {
                pend("unsupported", "bundled sources are not supported yet; provide an archive with `collection resolve --archive` or skip it");
            } else {
                pend("manual_download", m.source.instructions.empty() ? std::string("download this file manually and give it with `collection resolve --archive`") : m.source.instructions);
            }
            if (archive.empty()) {
                finish();
                continue;
            }
        }
        // 2) 校验
        if (!user_archive && !m.source.md5.empty()) {
            if (lower(md5_file(archive)) != lower(m.source.md5)) {
                std::error_code e2;
                fs::remove(archive, e2);  // 坏文件不留，下次重新下载
                fail("md5 mismatch for " + fs::path(archive).filename().string() + " (the file was deleted; run again to re-download)");
                finish();
                continue;
            }
        }
        ms.archive = archive;

        // 3) 安装
        try {
            mol::InstallOptions io;
            io.name = m.name;
            io.profile = profile;
            io.fomod_env.game_version = std::string(game_version.empty() ? "0.0.0.0" : game_version);
            bool need_choices = false;
            io.fomod_env.file_state = [base = std::function<std::string(std::string_view)>{}, &inst, &profile, &collection_plugins](std::string_view f) mutable {
                if (f.find_first_of("/\\") == std::string_view::npos && collection_plugins.count(lower(f))) return std::string("Active");
                if (!base) base = fomod_file_state(inst, profile);  // 遍历全部 mod：只在真有 FOMOD 条件时才建
                return base(f);
            };
            if (ov.has_choices) { io.fomod = FomodMode::Choices; io.choices = ov.choices; }
            else if (m.has_choices) { io.fomod = FomodMode::Choices; io.choices = m.choices; io.fomod_lenient = true; }
            else if (replicates(m, ov)) { io.fomod = FomodMode::Replicate; io.replicate = m.hashes; }
            else if (ov.fomod_defaults || opt.fomod_defaults) io.fomod = FomodMode::Defaults;
            else io.fomod = FomodMode::Unset;
            // 目录名冲突（大小写不敏感、按清理后的名字比较）：不是我们装的同名目录 → 加后缀
            std::string dir = m.name;
            if (!ms.mod_dir.empty()) dir = ms.mod_dir;
            else if (mod_name_taken(inst, dir, profile)) dir += " [" + (m.source.tag.empty() ? std::to_string(idx) : m.source.tag) + "]";
            io.name = dir;
            io.replace_existing = reinstall;
            io.force_root = m.deploys_to_root();
            // 之前中断留下的半成品（modlist 里没有但目录存在）：不处理，交给 install_archive 报错，避免误删
            auto res = mol::install_archive(inst, archive, io);
            if (m.source.type == "nexus" && m.source.mod_id > 0) {  // MO2 兼容的来源信息，供「已安装？」判断与更新检查使用
                const std::string md = std::string(res.path);
                set_mod_meta(md, "gameName", m.domain.empty() ? std::string(nexus_game_domain(inst.cfg.game)) : m.domain);
                set_mod_meta(md, "modid", std::to_string(m.source.mod_id));
                set_mod_meta(md, "fileid", std::to_string(m.source.file_id));
                if (!m.version.empty()) set_mod_meta(md, "version", m.version);
                if (io.fomod == FomodMode::Replicate) set_mod_meta(md, "mol_fomod", replicate_fingerprint(io.replicate));
                else if (res.fomod) set_mod_meta(md, "mol_fomod", fomod_fingerprint(io.fomod == FomodMode::Choices ? &io.choices : nullptr, io.fomod == FomodMode::Defaults));
            }
            if (reinstall)
                if (auto it = state.overrides.find(key); it != state.overrides.end()) it->second.reinstall = false;
            // 同一个文件的别的安装（选择不同）：内容相同的文件改成共享数据块
            if (m.source.type == "nexus" && m.source.mod_id > 0) {
                for (const Installed* other : any_install_of(m)) {
                    if (fs::path(other->path) == fs::path(std::string(res.path))) continue;
                    const DedupeStats ds = dedupe_tree(fs::path(std::string(res.path)), fs::path(other->path));
                    rep.deduped_files += ds.files;
                    rep.deduped_bytes += ds.bytes;
                    if (ds.files) break;  // 一个参考够了（再对别的参考去重会重复计数）
                }
                auto& v = (*nexus_files)[{m.source.mod_id, m.source.file_id}];
                std::erase_if(v, [&](const Installed& i) { return fs::path(i.path) == fs::path(std::string(res.path)); });
                v.push_back({std::string(res.path), std::string(Ini::load(std::string(res.path) + "/meta.ini").get("General", "mol_fomod").value_or(""))});
            }
            if (io.fomod == FomodMode::Replicate && !res.missing.empty()) {
                std::string miss;
                for (std::size_t i = 0; i < res.missing.size() && i < 5; ++i) miss += (i ? ", " : "") + res.missing[i];
                if (res.missing.size() > 5) miss += ", …";
                res.fomod_notes.push_back(std::to_string(res.missing.size()) + " file(s) the collection lists are not in this archive: " + miss);
            }
            ms.status = "installed";
            ms.mod_dir = std::string(res.name);
            ms.note.clear();
            if (!res.fomod_notes.empty()) {  // 清单的选择与压缩包对不上的地方用了默认：装上，但要让人知道
                ms.note = "FOMOD choices adapted to this archive: ";
                for (std::size_t i = 0; i < res.fomod_notes.size(); ++i) ms.note += (i ? "; " : "") + res.fomod_notes[i];
                out.note = ms.note;
                rep.notes.push_back(m.name + ": " + ms.note);
            }
            out.status = "installed";
            out.mod_dir = ms.mod_dir;
            ++rep.installed;
            final_names.emplace_back(ms.mod_dir);
            (void)need_choices;
        } catch (const Error& e) {
            if (e.code == "fomod_choices_required")
                pend("fomod_choices", "the collection records no FOMOD choices for this mod (the curator expects you to pick): run `fomod inspect` on the archive, then "
                                     "`collection resolve --mod KEY --fomod FILE` (or --fomod-defaults); or accept the installer defaults for every such mod with "
                                     "`collection install SLUG --fomod-defaults`");
            else if (m.has_choices && e.code == "invalid_argument" && std::string(e.what()).rfind("FOMOD:", 0) == 0)
                pend("fomod_choices", std::string("the collection's FOMOD choices do not fit this archive: ") + e.what());
            else
                fail(std::string(e.code) + ": " + e.what());
        }
        finish();
    }

    if (rep.reused || rep.reflinked || rep.deduped_files) {
        char mb[32];
        std::snprintf(mb, sizeof mb, "%.1f MiB", static_cast<double>(rep.deduped_bytes) / (1024.0 * 1024.0));
        rep.notes.push_back("reuse: " + std::to_string(rep.reused) + " mod(s) already installed in this instance, " + std::to_string(rep.reflinked) +
                            " reflinked from another instance, " + std::to_string(rep.deduped_files) + " file(s) / " + mb + " shared with other installs of the same archive");
    }
    // 4) 优先级：按安装顺序放到 modlist 最高处
    if (!final_names.empty()) reorder_mods(inst, final_names, profile);
    // 5) 插件列表
    if (c.has_plugins) {
        std::vector<std::string> missing;
        rep.plugins_applied = apply_plugin_spec(inst, c, profile, {}, &missing);
        // 工具生成的插件：策展人跑过工具，清单只带了工具本身——缺它们不是装坏了，而是要跑一次工具
        std::vector<std::string> generated, really;
        for (const auto& p : missing) (tool_for_generated_plugin(p).empty() ? really : generated).push_back(p);
        for (const auto& p : generated)
            rep.notes.push_back(p + " is generated by a tool, not shipped by any mod: run " + tool_for_generated_plugin(p) + " once (`executables list`, then `run --exe …`)");
        if (!really.empty()) {
            std::string list;
            for (std::size_t i = 0; i < really.size() && i < 8; ++i) list += (i ? ", " : "") + really[i];
            if (really.size() > 8) list += ", …";
            rep.notes.push_back(std::to_string(really.size()) + " plugin(s) the collection enables are not installed (" + list +
                                "): a mod was installed incompletely; `collection verify` finds which, `mods find NAME --archives` where the file is");
        }
    }
    state.name = c.info.name;
    save_state(inst, state);
    return rep;
}

namespace {
std::string norm_rel(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string out;  // 去掉空段与 "." 段
    for (std::size_t i = 0; i <= p.size();) {
        std::size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (!seg.empty() && seg != ".") out += (out.empty() ? "" : "/") + seg;
        i = j + 1;
    }
    return lower(out);
}
std::set<std::string> files_under(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = norm_rel(it->path().lexically_relative(dir).generic_string());
        if (rel != "meta.ini") out.insert(rel);
    }
    return out;
}
}  // namespace

VerifyReport verify_collection(const Instance& inst, const Collection& c, const State& state, std::string_view profile_in,
                               const std::function<void(std::size_t, std::size_t, std::string_view)>& progress) {
    VerifyReport rep;
    const std::string profile = profile_in.empty() ? std::string(inst.cfg.profile) : std::string(profile_in);
    std::set<std::string> collection_plugins, collection_plugins_all;
    for (const auto& p : c.plugins) {
        if (p.enabled) collection_plugins.insert(lower(p.name));
        collection_plugins_all.insert(lower(p.name));
    }
    std::function<std::string(std::string_view)> base_state;
    fomod::Env env;
    env.game_version = "0.0.0.0";
    env.file_state = [&](std::string_view f) {
        if (f.find_first_of("/\\") == std::string_view::npos && collection_plugins.count(lower(f))) return std::string("Active");
        if (!base_state) base_state = fomod_file_state(inst, profile);
        return base_state(f);
    };
    std::size_t done = 0;
    for (const auto& m : c.mods) {
        ++done;
        const std::string key = m.key();
        const auto sit = state.mods.find(key);
        if (sit == state.mods.end() || sit->second.status != "installed" || sit->second.mod_dir.empty()) continue;
        const Override ov = state.overrides.count(key) ? state.overrides.at(key) : Override{};
        const bool replicate = !m.hashes.empty() && !ov.has_choices && !m.has_choices;
        const fomod::Choices* choices = ov.has_choices ? &ov.choices : m.has_choices ? &m.choices : nullptr;
        if (!replicate && !choices) continue;  // 没有 FOMOD 选择也没有 hashes：没什么可对照的
        if (progress) progress(done, c.mods.size(), m.name);
        const fs::path dir = fs::path(std::string(inst.mods_dir)) / sit->second.mod_dir;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) { ++rep.skipped; continue; }
        const std::set<std::string> actual = files_under(dir);
        std::set<std::string> expected;
        VerifyItem item{key, m.name, sit->second.mod_dir, replicate ? "replicate" : "fomod", {}, {}};
        if (replicate) {
            for (const auto& [p, h] : m.hashes)
                if (norm_rel(p) != "meta.ini") expected.insert(norm_rel(p));  // Vortex 自己的 meta.ini 也会被记进 hashes
            ++rep.checked;
            for (const auto& e : expected) if (!actual.count(e)) item.missing.push_back(e);  // 复刻只看「该有的在不在」
        } else {
            std::string archive = sit->second.archive;
            // reflink 过来 / 复用的 mod 没记压缩包：按大小 + md5 在下载目录里找
            if (archive.empty() || !fs::is_regular_file(archive, ec)) archive = find_cached(fs::path(std::string(inst.downloads_dir)), m.source);
            if (archive.empty()) { ++rep.skipped; continue; }
            std::optional<fomod::Config> cfg;
            std::vector<std::string> names;
            try {
                cfg = read_archive_fomod(inst, archive);
                if (cfg) names = list_archive(archive);
            } catch (const Error&) { ++rep.skipped; continue; }
            if (!cfg) { ++rep.skipped; continue; }
            // FOMOD 的源路径相对「模块根」= ModuleConfig.xml 所在 fomod/ 目录的上一级
            std::string root;
            for (const auto& n : names) {
                const std::string l = norm_rel(n);
                if (l.ends_with("fomod/moduleconfig.xml")) { root = l.substr(0, l.size() - std::string("fomod/moduleconfig.xml").size()); break; }
            }
            std::vector<std::string> rels;  // 相对模块根（小写）
            for (const auto& n : names) {
                const std::string l = norm_rel(n);
                if (l.starts_with(root)) rels.push_back(l.substr(root.size()));
            }
            std::vector<std::string> notes;
            fomod::Resolved r;
            try { r = fomod::resolve(*cfg, *choices, false, env, &notes); } catch (const Error&) { ++rep.skipped; continue; }
            for (const auto& f : r.files) {
                const std::string src = norm_rel(f.source);
                std::string dst = f.destination;
                const bool dir_like = !dst.empty() && (dst.back() == '/' || dst.back() == '\\');
                dst = norm_rel(dst);
                if (f.folder) {
                    for (const auto& rel : rels)
                        if (rel.size() > src.size() && rel.starts_with(src + "/"))
                            expected.insert(dst.empty() ? rel.substr(src.size() + 1) : dst + "/" + rel.substr(src.size() + 1));
                } else {
                    const auto slash = src.find_last_of('/');
                    const std::string base = slash == std::string::npos ? src : src.substr(slash + 1);
                    expected.insert(dst.empty() ? base : dir_like ? dst + "/" + base : dst);
                }
            }
            // 安装时整体只有一个 Data/ 会被剥掉
            if (!expected.empty() && std::all_of(expected.begin(), expected.end(), [](const std::string& e) { return e.starts_with("data/"); })) {
                std::set<std::string> stripped;
                for (const auto& e : expected) stripped.insert(e.substr(5));
                expected.swap(stripped);
            }
            expected.erase("meta.ini");  // 我们自己的 meta.ini 会覆盖它
            // 顶层插件：只有清单插件列表里有的才算「应有」——FOMOD 的条件按今天的环境求值，可能比策展人当时多选；
            // 列表里没有的说明策展人那里没有这个文件
            std::erase_if(expected, [&](const std::string& e) {
                if (e.find('/') != std::string::npos) return false;
                const bool plugin = e.ends_with(".esp") || e.ends_with(".esm") || e.ends_with(".esl");
                return plugin && !actual.count(e) && !collection_plugins_all.count(e);
            });
            ++rep.checked;
            for (const auto& e : expected) if (!actual.count(e)) item.missing.push_back(e);
            for (const auto& a : actual) if (!expected.count(a)) item.extra.push_back(a);
        }
        if (!item.missing.empty() || !item.extra.empty()) rep.mismatched.push_back(std::move(item));
    }
    // 清单启用、磁盘上没有的插件（不改任何文件：只读插件列表）
    {
        const PluginList list = load_plugins(inst, {}, profile);
        std::set<std::string> have;
        for (const auto& r : list.rows) have.insert(lower(r.name));
        std::set<std::string> seen;
        for (const auto& p : c.plugins)
            if (p.enabled && seen.insert(lower(p.name)).second && !have.count(lower(p.name))) rep.missing_plugins.push_back(p.name);
    }
    return rep;
}

std::size_t apply_plugin_spec(const Instance& inst, const Collection& c, std::string_view profile, std::span<const string> forced,
                              std::vector<std::string>* missing) {
    if (!c.has_plugins || c.plugins.empty()) return 0;
    PluginList list = load_plugins(inst, forced, profile);
    std::vector<std::size_t> slots;
    std::vector<std::string> wanted;
    std::size_t applied = 0;
    std::set<std::string> seen;  // 清单的插件列表可能有重复（Constellations 有 35 个）：只认第一次出现，否则重复项占掉槽位、把末尾的插件挤出列表
    for (const auto& spec : c.plugins) {
        const auto want = casefold(spec.name);
        if (!seen.insert(std::string(want)).second) continue;
        bool found = false;
        for (std::size_t i = 0; i < list.rows.size(); ++i) {
            if (casefold(list.rows[i].name) != want) continue;
            if (!list.rows[i].forced) {
                list.rows[i].enabled = spec.enabled;
                wanted.push_back(std::string(list.rows[i].name));
            }
            ++applied;
            found = true;
            break;
        }
        if (!found && spec.enabled && missing) missing->push_back(spec.name);
    }
    // 在这些插件原有的槽位里，按清单顺序重新排列它们
    for (std::size_t i = 0; i < list.rows.size(); ++i)
        for (const auto& w : wanted) if (casefold(w) == casefold(list.rows[i].name)) { slots.push_back(i); break; }
    std::vector<PluginRow> reordered;
    for (const auto& w : wanted)
        for (const auto& r : list.rows) if (casefold(r.name) == casefold(w)) { reordered.push_back(r); break; }
    for (std::size_t k = 0; k < slots.size() && k < reordered.size(); ++k) list.rows[slots[k]] = reordered[k];
    plugin_sort_by_masters(list);
    save_plugins(inst, list, profile);
    return applied;
}

}  // namespace mol::collection
