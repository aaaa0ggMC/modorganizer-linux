// envelope 序列化（alib6 反射 + 紧凑 JSON）与文本渲染。
//
// 本文件不 #include 任何标准库头：import std / import alib6 之后即可（混用约束见 cmd_common.hpp）。
#include "output.hpp"

import alib6;
import std;

namespace cli {

using namespace alib6;

namespace {

// 文本渲染用：AData 里按 key 取字符串（缺失 → 空）
std::string S(const alib6::AData& node, std::string_view key) {
    return std::string(adata::str(node, key));
}

void add(std::pmr::vector<std::pmr::string>& lines, const std::string& text, mol::mr* mem) {
    lines.push_back(mol::string(std::string_view(text), mem));
}

std::string human(long long n) {
    if (n < 0) return "?";
    const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    char buf[32];
    std::snprintf(buf, sizeof buf, i == 0 ? "%.0f %s" : "%.2f %s", v, u[i]);
    return buf;
}

const alib6::AData* arr(const alib6::AData& d, std::string_view key) {
    const alib6::AData* a = adata::field(d, key);
    return a && a->is_array() ? a : nullptr;
}

void render_script_run(const alib6::AData& d, std::pmr::vector<std::pmr::string>& lines, mol::mr* mem) {
    const bool ok = adata::boolean(d, "ok");
    add(lines, std::string("script ") + (ok ? "ok" : "FAILED") + ": " + S(d, "script"), mem);
    add(lines, "  namespace: " + S(d, "ns") + " (" + S(d, "mode") + ")", mem);
    add(lines, "  virtual root: " + S(d, "root"), mem);
    if (const std::string url = S(d, "http_url"); !url.empty()) add(lines, "  state: " + url, mem);
    if (const std::string ll = S(d, "landlock"); !ll.empty())
        add(lines, "  exe containment (Landlock): " + ll, mem);
    if (!ok) add(lines, "  error: " + S(d, "error"), mem);
    if (const alib6::AData* log = adata::field(d, "log")) {
        const std::size_t n = adata::size(*log);
        const std::size_t shown = std::min<std::size_t>(n, 20);
        for (std::size_t i = 0; i < shown; ++i)
            if (const alib6::AData* l = adata::at(*log, i)) add(lines, "  | " + std::string(adata::str(*l, "")), mem);
        if (n > shown) add(lines, "  | ... (" + std::to_string(n - shown) + " more)", mem);
    }
    if (const alib6::AData* st = adata::field(d, "state")) {
        for (std::size_t i = 0, n = adata::size(*st); i < n; ++i)
            if (const alib6::AData* s = adata::at(*st, i))
                add(lines, "  state " + S(*s, "key") + " = " + S(*s, "value"), mem);
    }
}

void render_instance_show(const alib6::AData& d, std::pmr::vector<std::pmr::string>& lines,
                          mol::mr* mem) {
    add(lines, "instance " + S(d, "root"), mem);
    for (const char* key : {"mods_dir", "profiles_dir", "downloads_dir", "overwrite_dir",
                            "farm_path"}) {
        add(lines, std::string(key) + ": " + S(d, key), mem);
    }
    add(lines, "config:", mem);
    if (const alib6::AData* cfg = adata::field(d, "config")) {
        if (cfg->is_object()) {
            for (auto it : cfg->object()) {
                add(lines, "  " + std::string(it.first()) + ": " + S(*cfg, it.first()), mem);
            }
        }
    }
}

void render_mods_list(const alib6::AData& d, std::pmr::vector<std::pmr::string>& lines, mol::mr* mem) {
    const alib6::AData* mods = adata::field(d, "mods");
    const std::size_t n = mods == nullptr ? 0 : adata::size(*mods);
    add(lines, "profile " + S(d, "profile") + ": " + std::to_string(n) + " mod(s)", mem);
    if (mods == nullptr) return;
    for (std::size_t i = 0; i < n; ++i) {
        const alib6::AData* m = adata::at(*mods, i);
        if (m == nullptr) continue;
        const bool enabled = adata::boolean(*m, "enabled");
        const bool separator = adata::boolean(*m, "separator");
        const bool exists = adata::boolean(*m, "exists");
        const long long priority = adata::integer(*m, "priority");
        char tag[16];
        std::snprintf(tag, sizeof(tag), "%03lld", priority);
        std::string line = "  [" + std::string(enabled ? "x" : " ") + "] " + tag + " " + S(*m, "name");
        if (separator) line += " (separator)";
        if (!exists && !separator) line += " (missing)";
        const std::string path = S(*m, "path");
        if (!path.empty()) line += " -> " + path;
        add(lines, line, mem);
    }
}

void render_conflicts(const alib6::AData& d, std::pmr::vector<std::pmr::string>& lines, mol::mr* mem) {
    const alib6::AData* list = adata::field(d, "conflicts");
    const std::size_t n = list == nullptr ? 0 : adata::size(*list);
    add(lines, std::to_string(adata::integer(d, "count")) + " conflict(s) over " +
                   std::to_string(n) + " path(s)",
        mem);
    if (list == nullptr) return;
    std::size_t shown = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (shown++ >= 20) {
            add(lines, "  ... (truncated)", mem);
            break;
        }
        const alib6::AData* c = adata::at(*list, i);
        if (c == nullptr) continue;
        std::string line = "  " + S(*c, "path") + " -> " + S(*c, "winner") + " (over:";
        if (const alib6::AData* losers = adata::field(*c, "losers")) {
            const std::size_t ln = adata::size(*losers);
            for (std::size_t j = 0; j < ln; ++j) {
                const alib6::AData* l = adata::at(*losers, j);
                line += (j == 0 ? " " : ",");
                if (l != nullptr && l->is_value()) line += std::string(l->value().to<std::string_view>());
            }
        }
        line += ")";
        add(lines, line, mem);
    }
}

}  // namespace

Envelope make_envelope(const Result& r, mol::mr* mem) {
    Envelope env{mol::allocator_type(mem)};
    env.schema_version = 1;
    env.ok = r.ok;
    env.command = r.command;
    env.data = r.data;  // AData 拷贝（ok=false 时 r.data 为 null）
    env.errors = r.errors;
    env.warnings = r.warnings;
    return env;
}

std::pmr::string serialize_envelope(const Result& r, mol::mr* mem) {
    const Envelope env = make_envelope(r, mem);
    AData doc = to_adata(env, mem);

    JSONConfig cfg;
    cfg.compact_lines = true;   // 单行
    cfg.compact_spaces = true;  // 去掉 ": " / ", " 的空格
    cfg.sort_object = JSONConfig::sort_asc;  // 键按字典序 ⇒ 输出确定
    JSON json(cfg);
    std::string out;
    json.dump(out, doc);
    return mol::string(out, mem);
}

std::pmr::vector<std::pmr::string> render_text(const Result& r, mol::mr* mem) {
    std::pmr::vector<std::pmr::string> lines(mem);
    if (!r.ok) return lines;  // 失败只在 stderr 输出诊断（render_diagnostics）

    const std::string cmd = std::string(r.command);
    if (cmd == "version") {
        add(lines, S(r.data, "name") + " " + S(r.data, "version"), mem);
    } else if (cmd == "game describe") {
        add(lines, S(r.data, "id") + ": " + S(r.data, "executable"), mem);
        add(lines, "data " + S(r.data, "data_directory") + " (" + S(r.data, "plugin_format") + ")", mem);
    } else if (cmd == "game info") {
        add(lines, S(r.data, "name") + " " + S(r.data, "version"), mem);
        add(lines, "game " + S(r.data, "gameDirectory"), mem);
        add(lines, "data " + S(r.data, "dataDirectory"), mem);
        add(lines, "binary " + S(r.data, "binaryName"), mem);
    } else if (cmd == "next") {
        add(lines, adata::boolean(r.data, "ready") ? "ready" : "not ready yet", mem);
        for (const auto& st : r.data.object().find("steps").second().array()) {
            std::string c;
            for (const auto& a : st.object().find("command").second().array()) c += " " + std::string(a.try_to<std::string_view>().value_or(""));
            std::string line = std::string(adata::boolean(st, "blocking") ? "  * " : "  - ") + S(st, "id") + ": " + S(st, "why");
            if (!c.empty()) line += "\n      mo-linux" + c;
            if (adata::boolean(st, "needs_human")) line += "   [needs the user]";
            if (adata::boolean(st, "confirm")) line += "   [confirm first]";
            add(lines, line, mem);
        }
    } else if (cmd == "docs") {
        if (!S(r.data, "topic").empty()) {
            add(lines, S(r.data, "markdown"), mem);
        } else {
            add(lines, "built-in documentation (mo-linux docs TOPIC):", mem);
            for (const auto& t : r.data.object().find("topics").second().array()) {
                std::string name = S(t, "name");
                name.resize(10, ' ');
                add(lines, "  " + name + S(t, "title"), mem);
            }
        }
    } else if (cmd == "nexus whoami" || cmd == "nexus login") {
        // Premium 决定集合能否自动下载：文本模式也要直接看到
        const bool premium = adata::boolean(r.data, "is_premium");
        add(lines, S(r.data, "name") + " (user " + std::to_string(adata::integer(r.data, "user_id")) + ") — " +
                       (premium ? "Premium: downloads run automatically" : "free account: each download needs an nxm:// link from the website (`mo-linux nxm register`)") +
                       (adata::boolean(r.data, "is_supporter") && !premium ? ", supporter" : ""), mem);
        if (!S(r.data, "key_path").empty()) add(lines, "key saved to " + S(r.data, "key_path"), mem);
    } else if (cmd == "collection inspect") {
        add(lines, S(r.data, "name") + " (" + S(r.data, "slug") + ", revision " + std::to_string(adata::integer(r.data, "revision")) + ") by " + S(r.data, "author"), mem);
        if (!S(r.data, "url").empty()) add(lines, S(r.data, "url"), mem);
        std::string sz = std::to_string(adata::integer(r.data, "mod_count")) + " mods, " + human(adata::integer(r.data, "total_size")) + " to download";
        if (adata::integer(r.data, "optional_size") > 0) sz += " (" + human(adata::integer(r.data, "optional_size")) + " of it optional)";
        if (adata::integer(r.data, "declared_total_size") > 0) sz += "; the page declares " + human(adata::integer(r.data, "declared_total_size"));
        add(lines, sz, mem);
        if (const auto* src = arr(r.data, "sources")) {
            std::string s = "sources:";
            for (const auto& x : src->array()) s += " " + S(x, "type") + " " + std::to_string(adata::integer(x, "count")) + " (" + human(adata::integer(x, "size")) + ")";
            add(lines, s, mem);
        }
        std::string gvs;
        if (const auto* g = arr(r.data, "game_versions"))
            for (const auto& v : g->array()) gvs += (gvs.empty() ? "" : ", ") + std::string(v.try_to<std::string_view>().value_or(""));
        if (!gvs.empty()) add(lines, "game version: " + gvs + (S(r.data, "game_version").empty() ? "" : " (this game: " + S(r.data, "game_version") + ")"), mem);
        if (adata::integer(r.data, "fomod_choices") > 0)
            add(lines, std::to_string(adata::integer(r.data, "fomod_choices")) + " mods carry the curator's FOMOD choices (applied leniently: where the archive no longer matches, "
                       "the installer default is used and noted); FOMODs without recorded choices become pending (or use --fomod-defaults)", mem);
        if (!adata::boolean(r.data, "has_instance")) add(lines, "(no instance: install status not shown)", mem);
        if (!S(r.data, "description").empty()) add(lines, "read the author's notes first: mo-linux collection readme " + S(r.data, "slug"), mem);
    } else if (cmd == "collection search") {
        if (const auto* rows = arr(r.data, "collections"))
            for (const auto& c : rows->array())
                add(lines, "  " + S(c, "slug") + "  " + S(c, "name") + "  — " + std::to_string(adata::integer(c, "mod_count")) + " mods, ~" +
                               human(adata::integer(c, "total_size")) + " declared (real download is often larger: see `collection inspect`)", mem);
    } else if (cmd == "collection install" || cmd == "collection status") {
        add(lines, S(r.data, "name") + ": " + S(r.data, "status") + " — " + std::to_string(adata::integer(r.data, "installed")) + " installed, " +
                       std::to_string(adata::integer(r.data, "skipped")) + " skipped, " + std::to_string(adata::integer(r.data, "failed")) + " failed", mem);
        if (const auto* pend = arr(r.data, "pending"))
            for (const auto& p : pend->array())
                add(lines, "  pending [" + S(p, "kind") + "] " + S(p, "key") + "  " + S(p, "name") + (S(p, "url").empty() ? "" : "  " + S(p, "url")), mem);
    } else if (cmd == "collection readme") {
        add(lines, S(r.data, "markdown"), mem);
    } else if (cmd == "schema") {
        for (const auto& c : r.data.object().find("commands").second().array()) add(lines, S(c, "name") + " - " + S(c, "summary"), mem);
    } else if (cmd == "logs") {
        add(lines, "log directory: " + S(r.data, "dir"), mem);
        if (S(r.data, "name").empty()) {
            for (const auto& f : r.data.object().find("files").second().array()) add(lines, "  " + S(f, "name") + "  (" + std::to_string(adata::integer(f, "size")) + " bytes)", mem);
        } else {
            add(lines, S(r.data, "tail"), mem);
        }
    } else if (cmd == "doctor") {
        const auto& checks = r.data.object().find("checks").second().array();
        for (const auto& c : checks) {
            const auto& o = c.object();
            const std::string lvl = S(c, "level");
            std::string line = (lvl == "ok" ? "[ ok ] " : lvl == "warn" ? "[warn] " : "[FAIL] ") + S(c, "message");
            const std::string hint = S(c, "hint");
            if (!hint.empty()) line += "  -> " + hint;
            (void)o;
            add(lines, line, mem);
        }
        add(lines, std::to_string(adata::integer(r.data, "errors")) + " error(s), " +
                       std::to_string(adata::integer(r.data, "warnings")) + " warning(s)", mem);
    } else if (cmd == "run") {
        if (adata::boolean(r.data, "dry_run")) add(lines, "run --dry-run: " + S(r.data, "exe"), mem);
        else if (adata::boolean(r.data, "detached")) add(lines, "run: started " + S(r.data, "exe"), mem);
        else add(lines, "run: " + S(r.data, "exe") + " exited with " + std::to_string(adata::integer(r.data, "game_exit_code")), mem);
        const alib6::AData* dg = adata::field(r.data, "diagnosis");
        const std::size_t n = dg == nullptr ? 0 : adata::size(*dg);
        if (n > 0) add(lines, "diagnosis (" + S(r.data, "log_file") + "):", mem);
        for (std::size_t i = 0; i < n; ++i)
            if (const alib6::AData* x = adata::at(*dg, i)) if (auto v = x->try_to<std::string_view>()) add(lines, "  " + std::string(*v), mem);
    } else if (cmd == "collection verify") {
        const alib6::AData* ms = adata::field(r.data, "mismatched");
        const std::size_t n = ms == nullptr ? 0 : adata::size(*ms);
        for (std::size_t i = 0; i < n; ++i) {
            const alib6::AData* m = adata::at(*ms, i);
            if (m == nullptr) continue;
            std::string line = "  [" + std::string(adata::str(*m, "kind")) + "] " + std::string(adata::str(*m, "mod_dir")) + ": " +
                               std::to_string(adata::integer(*m, "missing_count")) + " missing, " + std::to_string(adata::integer(*m, "extra_count")) + " extra";
            const alib6::AData* miss = adata::field(*m, "missing");
            if (miss != nullptr && adata::size(*miss) > 0)
                if (const alib6::AData* x = adata::at(*miss, 0)) if (auto v = x->try_to<std::string_view>()) line += " (e.g. " + std::string(*v) + ")";
            add(lines, line, mem);
        }
        if (const alib6::AData* mp = adata::field(r.data, "missing_plugins"); mp && adata::size(*mp) > 0) {
            std::string l = "  enabled in the collection but not installed:";
            for (std::size_t i = 0; i < adata::size(*mp); ++i)
                if (const alib6::AData* x = adata::at(*mp, i)) if (auto v = x->try_to<std::string_view>()) l += (i ? ", " : " ") + std::string(*v);
            add(lines, l, mem);
        }
        add(lines, "collection verify: " + std::to_string(adata::integer(r.data, "checked")) + " checked, " + std::to_string(adata::integer(r.data, "skipped")) +
                       " skipped, " + std::to_string(n) + " differ" + (adata::integer(r.data, "marked") > 0 ? ", " + std::to_string(adata::integer(r.data, "marked")) + " marked for reinstall" : ""), mem);
    } else if (cmd == "mods find") {
        const alib6::AData* hs = adata::field(r.data, "hits");
        const std::size_t n = hs == nullptr ? 0 : adata::size(*hs);
        for (std::size_t i = 0; i < n; ++i) {
            const alib6::AData* h = adata::at(*hs, i);
            if (h == nullptr) continue;
            const bool arc = adata::boolean(*h, "in_archive");
            add(lines, std::string(arc ? "archive " : adata::boolean(*h, "enabled") ? "mod     " : "mod (disabled) ") + std::string(adata::str(*h, "where")) + " : " +
                           std::string(adata::str(*h, "path")), mem);
        }
        if (n == 0) add(lines, "mods find: " + S(r.data, "file") + " not found" + (adata::boolean(r.data, "searched_archives") ? "" : " in any mod (add --archives to search downloads/)"), mem);
    } else if (cmd == "terminate") {
        const bool dry = adata::boolean(r.data, "dry_run");
        const alib6::AData* ps = adata::field(r.data, "processes");
        const std::size_t n = ps == nullptr ? 0 : adata::size(*ps);
        for (std::size_t i = 0; i < n; ++i) {
            const alib6::AData* p = adata::at(*ps, i);
            if (p == nullptr) continue;
            add(lines, std::string(dry ? "would end " : "  ") + std::to_string(adata::integer(*p, "pid")) + "  [" + std::string(adata::str(*p, "reason")) + "] " +
                           std::string(adata::str(*p, "command")), mem);
        }
        auto count = [&](const char* k) {
            const alib6::AData* a = adata::field(r.data, k);
            return a == nullptr ? std::size_t{0} : adata::size(*a);
        };
        if (n == 0) add(lines, "terminate: nothing is using this instance", mem);
        else if (dry) add(lines, "terminate --dry-run: " + std::to_string(n) + " process(es)", mem);
        else
            add(lines, "terminate: " + std::to_string(count("terminated")) + " exited, " + std::to_string(count("killed")) + " killed, " + std::to_string(count("remaining")) +
                           " still running" + (adata::boolean(r.data, "wineserver_killed") ? "; wineserver stopped" : ""), mem);
    } else if (cmd == "fix content-catalog" || cmd == "fix vcrun" || cmd == "enb install") {
        add(lines, cmd + ": " + S(r.data, "message"), mem);
    } else if (cmd == "plugins sync") {
        add(lines, std::string("plugins sync ") + (adata::boolean(r.data, "changed") ? "(changed)" : "(unchanged)"), mem);
    } else if (cmd == "instance init") {
        const bool changed = adata::boolean(r.data, "changed");
        add(lines, "instance initialized at " + S(r.data, "root") +
                       (changed ? " (changed)" : " (unchanged)"),
            mem);
    } else if (cmd == "instance show") {
        render_instance_show(r.data, lines, mem);
    } else if (cmd == "mods list") {
        render_mods_list(r.data, lines, mem);
    } else if (cmd == "mods enable" || cmd == "mods disable") {
        const bool enabled = adata::boolean(r.data, "enabled");
        const bool changed = adata::boolean(r.data, "changed");
        add(lines, "mod '" + S(r.data, "name") + "' " + (enabled ? "enabled" : "disabled") +
                       (changed ? " (changed)" : " (unchanged)"),
            mem);
    } else if (cmd == "mods move") {
        const bool changed = adata::boolean(r.data, "changed");
        add(lines, "mod '" + S(r.data, "name") + "' moved to priority " +
                       std::to_string(adata::integer(r.data, "priority")) +
                       (changed ? " (changed)" : " (unchanged)"),
            mem);
    } else if (cmd == "conflicts") {
        render_conflicts(r.data, lines, mem);
    } else if (cmd == "script run") {
        render_script_run(r.data, lines, mem);
    } else if (cmd == "mods impact") {
        add(lines, S(r.data, "mod") + ": " + S(r.data, "summary"), mem);
        if (adata::boolean(r.data, "packed_suspect"))
            add(lines, "  note: the DLL looks packed (writable+executable section or no imports); "
                       "static analysis is an upper bound only",
                mem);
        if (const alib6::AData* inj = adata::field(r.data, "injections")) {
            const std::size_t n = adata::size(*inj);
            const std::size_t shown = std::min<std::size_t>(n, 24);
            for (std::size_t i = 0; i < shown; ++i)
                if (const alib6::AData* row = adata::at(*inj, i))
                    add(lines, "  " + S(*row, "kind") + "  " + S(*row, "path") + "  (loaded by " +
                                   S(*row, "loaded_by") + ", reach: " + S(*row, "reach") + ")",
                        mem);
            if (n > shown) add(lines, "  ... (" + std::to_string(n - shown) + " more)", mem);
        }
        std::string caps;
        if (const alib6::AData* c = adata::field(r.data, "caps")) {
            auto has = [&caps, c](const char* k, const char* label) {
                if (adata::boolean(*c, k)) caps += (caps.empty() ? "" : ", ") + std::string(label);
            };
            has("writes_files", "writes files");
            has("spawns_processes", "spawns processes");
            has("network", "network");
            has("registry", "registry");
            has("memory_patch", "memory patch");
            has("chain_loads", "loads DLLs");
            has("unknown", "unknown");
        }
        if (caps.empty()) caps = "(no notable capabilities)";
        add(lines, "  caps: " + caps, mem);
        if (const alib6::AData* ev = adata::field(r.data, "evidence"))
            for (std::size_t i = 0, n = std::min<std::size_t>(adata::size(*ev), 8); i < n; ++i)
                if (const alib6::AData* e = adata::at(*ev, i))
                    add(lines, "    " + std::string(adata::str(*e, "")), mem);
    } else if (cmd == "serve") {
        if (const std::string http = S(r.data, "http"); !http.empty())
            add(lines, "serve: listening on " + http + " (control socket " + S(r.data, "socket") + ")", mem);
        else
            add(lines, "serve: asked the service on " + S(r.data, "socket") + " to stop", mem);
    } else if (cmd == "plan") {
        std::string line = "plan: " + std::to_string(adata::integer(r.data, "count")) + " op(s) [";
        if (const alib6::AData* counts = adata::field(r.data, "counts")) {
            line += "mkdir=" + std::to_string(adata::integer(*counts, "mkdir")) +
                    " link=" + std::to_string(adata::integer(*counts, "link")) +
                    " relink=" + std::to_string(adata::integer(*counts, "relink")) +
                    " remove=" + std::to_string(adata::integer(*counts, "remove")) +
                    " rmdir=" + std::to_string(adata::integer(*counts, "rmdir"));
        }
        line += "], warnings=" + std::to_string(adata::integer(r.data, "warnings"));
        add(lines, line, mem);
        if (const alib6::AData* ops = adata::field(r.data, "ops")) {
            const std::size_t n = adata::size(*ops);
            std::size_t shown = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (shown++ >= 20) {
                    add(lines, "  ... (truncated)", mem);
                    break;
                }
                const alib6::AData* op = adata::at(*ops, i);
                if (op == nullptr) continue;
                std::string line2 = "  " + S(*op, "kind") + " " + S(*op, "path");
                const std::string target = S(*op, "target");
                if (!target.empty()) line2 += " -> " + target;
                add(lines, line2, mem);
            }
        }
    } else if (cmd == "status") {
        if (adata::boolean(r.data, "in_sync")) {
            add(lines, "in sync", mem);
        } else {
            add(lines, "drift: " + std::to_string(adata::integer(r.data, "pending")) +
                           " pending op(s)",
                mem);
        }
        add(lines, "farm " + S(r.data, "farm_path") +
                       (adata::boolean(r.data, "farm_exists") ? " (present)" : " (absent)"),
            mem);
    } else if (cmd == "apply") {
        const bool changed = adata::boolean(r.data, "changed");
        add(lines, "applied " + std::to_string(adata::integer(r.data, "applied")) + " op(s)" +
                       (changed ? " (changed)" : " (nothing changed)"),
            mem);
        add(lines, "farm " + S(r.data, "farm_path"), mem);
    } else if (cmd == "unlink") {
        add(lines, adata::boolean(r.data, "removed") ? "farm removed" : "farm already absent", mem);
        add(lines, "farm " + S(r.data, "farm_path"), mem);
    } else {
        add(lines, cmd + ": ok", mem);
    }
    return lines;
}

std::pmr::vector<std::pmr::string> render_diagnostics(const Result& r, mol::mr* mem) {
    std::pmr::vector<std::pmr::string> lines(mem);
    for (const Err& w : r.warnings) {
        std::string line = "warning[" + std::string(w.code) + "]: " + std::string(w.message);
        if (!w.path.empty()) line += " (" + std::string(w.path) + ")";
        add(lines, line, mem);
    }
    for (const Err& e : r.errors) {
        std::string line = "error[" + std::string(e.code) + "]: " + std::string(e.message);
        if (!e.path.empty()) line += " (" + std::string(e.path) + ")";
        if (!e.hint.empty()) line += "\n  hint: " + std::string(e.hint);
        add(lines, line, mem);
    }
    return lines;
}

}  // namespace cli
