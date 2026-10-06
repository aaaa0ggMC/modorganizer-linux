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
    for (const auto& step : opts->array()) {
        const std::string sname = S(step, "name");
        const auto* groups = sub(step, "groups");
        if (!groups || !groups->is_array()) continue;
        for (const auto& g : groups->array()) {
            auto& set = out[sname][S(g, "name")];
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
        c.info.name = S(*info, "name");
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
        mod.name = S(m, "name");
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

// modlist 里已有的 Nexus 文件：(modid, fileid) → mod 目录名（来自 meta.ini）。
std::map<std::pair<std::int64_t, std::int64_t>, std::string> index_nexus_files(const Instance& inst, std::string_view profile) {
    std::map<std::pair<std::int64_t, std::int64_t>, std::string> out;
    for (const auto& md : list_mods(inst, profile)) {
        if (md.separator || !md.exists || md.nexus_id <= 0) continue;
        const Ini meta = Ini::load(std::string(md.path) + "/meta.ini");
        const auto fid = meta.get("General", "fileid");
        std::int64_t f = 0;
        if (!fid || fid->empty()) continue;
        for (char ch : *fid) { if (ch < '0' || ch > '9') { f = 0; break; } f = f * 10 + (ch - '0'); }
        if (f > 0) out.emplace(std::make_pair(md.nexus_id, f), fs::path(std::string(md.path)).filename().string());
    }
    return out;
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

    // ---- 预取阶段：把需要联网下载的压缩包并行下完（安装仍按顺序串行进行）----
    std::map<std::string, std::string> cached;    // key → 本地已有的压缩包（不需要下载）
    std::map<std::string, Fetched> fetched;       // key → 并行下载的结果
    {
        std::vector<std::size_t> todo;
        for (const std::size_t idx : order) {
            const Mod& m = c.mods[idx];
            const std::string key = m.key();
            const auto sit = state.mods.find(key);
            if (sit != state.mods.end() && sit->second.status == "installed" && !sit->second.mod_dir.empty() &&
                fs::is_directory(fs::path(std::string(inst.mods_dir)) / sit->second.mod_dir, ec))
                continue;
            const Override ov = state.overrides.count(key) ? state.overrides.at(key) : Override{};
            if (ov.skip || (m.optional && !opt.include_optional) || (m.has_patches && ov.archive.empty()) || !ov.archive.empty()) continue;
            if (sit != state.mods.end() && !sit->second.archive.empty() && fs::is_regular_file(sit->second.archive, ec)) continue;
            if (auto p = find_cached(downloads, m.source); !p.empty()) { cached[key] = p; continue; }
            if (client && (m.source.type == "nexus" || (m.source.type == "direct" && !m.source.url.empty()))) todo.push_back(idx);
        }
        if (!todo.empty()) {
            std::uint64_t total = 0;
            for (auto idx : todo) total += static_cast<std::uint64_t>(std::max<std::int64_t>(c.mods[idx].source.file_size, 0));
            std::vector<std::atomic<std::uint64_t>> cur(todo.size());
            std::mutex mu;
            std::size_t finished = 0;
            parallel_for(todo.size(), download_jobs(opt.jobs), [&](std::size_t i) {
                const Mod& m = c.mods[todo[i]];
                Fetched f = fetch_one(inst, client, m, [&](std::uint64_t d, std::uint64_t) {
                    cur[i].store(d);
                    if (!opt.progress) return;
                    std::uint64_t sum = 0;
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

    std::optional<std::map<std::pair<std::int64_t, std::int64_t>, std::string>> nexus_files;  // 首次用到时建
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

        // 已完成
        if (ms.status == "installed" && !ms.mod_dir.empty() && fs::is_directory(fs::path(std::string(inst.mods_dir)) / ms.mod_dir, ec)) {
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
        if (ov.archive.empty() && m.source.type == "nexus" && m.source.mod_id > 0 && m.source.file_id > 0) {
            if (!nexus_files) nexus_files = index_nexus_files(inst, profile);
            if (auto hit = nexus_files->find({m.source.mod_id, m.source.file_id}); hit != nexus_files->end()) {
                const std::string dir = hit->second;
                ms.status = "installed";
                ms.mod_dir = dir;
                ms.note = "already installed (same Nexus file)";
                out.status = "installed";
                out.mod_dir = dir;
                out.note = ms.note;
                ++rep.installed;
                final_names.emplace_back(dir);
                finish();
                continue;
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
            if (ov.has_choices) { io.fomod = FomodMode::Choices; io.choices = ov.choices; }
            else if (m.has_choices) { io.fomod = FomodMode::Choices; io.choices = m.choices; }
            else if (ov.fomod_defaults || opt.fomod_defaults) io.fomod = FomodMode::Defaults;
            else io.fomod = FomodMode::Unset;
            // 目录名冲突（大小写不敏感、按清理后的名字比较）：不是我们装的同名目录 → 加后缀
            std::string dir = m.name;
            if (!ms.mod_dir.empty()) dir = ms.mod_dir;
            else if (mod_name_taken(inst, dir, profile)) dir += " [" + (m.source.tag.empty() ? std::to_string(idx) : m.source.tag) + "]";
            io.name = dir;
            // 之前中断留下的半成品（modlist 里没有但目录存在）：不处理，交给 install_archive 报错，避免误删
            const auto res = mol::install_archive(inst, archive, io);
            if (m.source.type == "nexus" && m.source.mod_id > 0) {  // MO2 兼容的来源信息，供「已安装？」判断与更新检查使用
                const std::string md = std::string(res.path);
                set_mod_meta(md, "gameName", m.domain.empty() ? std::string(nexus_game_domain(inst.cfg.game)) : m.domain);
                set_mod_meta(md, "modid", std::to_string(m.source.mod_id));
                set_mod_meta(md, "fileid", std::to_string(m.source.file_id));
                if (!m.version.empty()) set_mod_meta(md, "version", m.version);
            }
            ms.status = "installed";
            ms.mod_dir = std::string(res.name);
            ms.note.clear();
            out.status = "installed";
            out.mod_dir = ms.mod_dir;
            ++rep.installed;
            final_names.emplace_back(ms.mod_dir);
            (void)need_choices;
        } catch (const Error& e) {
            if (e.code == "fomod_choices_required")
                pend("fomod_choices", "this FOMOD installer needs choices: run `fomod inspect` on the archive, then `collection resolve --mod KEY --fomod FILE` (or --fomod-defaults)");
            else if (m.has_choices && e.code == "invalid_argument" && std::string(e.what()).rfind("FOMOD:", 0) == 0)
                pend("fomod_choices", std::string("the collection's FOMOD choices do not fit this archive: ") + e.what());
            else
                fail(std::string(e.code) + ": " + e.what());
        }
        finish();
    }

    // 4) 优先级：按安装顺序放到 modlist 最高处
    if (!final_names.empty()) reorder_mods(inst, final_names, profile);
    // 5) 插件列表
    if (c.has_plugins) rep.plugins_applied = apply_plugin_spec(inst, c, profile);
    state.name = c.info.name;
    save_state(inst, state);
    return rep;
}

std::size_t apply_plugin_spec(const Instance& inst, const Collection& c, std::string_view profile, std::span<const string> forced) {
    if (!c.has_plugins || c.plugins.empty()) return 0;
    PluginList list = load_plugins(inst, forced, profile);
    std::vector<std::size_t> slots;
    std::vector<std::string> wanted;
    std::size_t applied = 0;
    for (const auto& spec : c.plugins) {
        const auto want = casefold(spec.name);
        for (std::size_t i = 0; i < list.rows.size(); ++i) {
            if (casefold(list.rows[i].name) != want) continue;
            if (!list.rows[i].forced) {
                list.rows[i].enabled = spec.enabled;
                wanted.push_back(std::string(list.rows[i].name));
            }
            ++applied;
            break;
        }
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
